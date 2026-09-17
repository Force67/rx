// The TLS half of rx::http, kept in its own translation unit so a build
// without an mbedTLS backend still links the plain-HTTP client (MakeTlsStream
// then returns null and an https URL fails with a clear error).

#include "http/stream.h"

#if RX_HTTP_TLS

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>  // only for the MBEDTLS_ERR_NET_* BIO codes
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "core/log.h"

namespace rx::http {
namespace {

// TLS 1.3 (and all of mbedTLS 4) draws its randomness from PSA, which the
// application has to start exactly once before the first handshake. Harmless
// on a 3.x build that only negotiates 1.2.
void EnsurePsaCrypto() {
  static std::once_flag once;
  std::call_once(once, [] {
    const psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS)
      RX_WARN("http: psa_crypto_init failed ({}), tls handshakes may fail", int(status));
  });
}

base::String MbedError(const char* what, int code) {
  char text[128] = {};
  mbedtls_strerror(code, text, sizeof(text));
  char buffer[256] = {};
  std::snprintf(buffer, sizeof(buffer), "%s: %s (-0x%04x)", what, text,
                static_cast<unsigned>(-code));
  return base::String(buffer);
}

// Where a Unix-like system keeps its CA bundle. Distributions disagree, so the
// first readable one wins; SSL_CERT_FILE overrides all of them, which is how
// NixOS and containers point at their own store.
const char* const kCaBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // debian, ubuntu, nixos, alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // fedora, rhel
    "/etc/ssl/ca-bundle.pem",              // opensuse
    "/etc/ssl/cert.pem",                   // macos, openbsd, freebsd
};

#ifdef _WIN32
// Windows has no CA file: the roots live in the "ROOT" system store, so they
// are enumerated and handed to mbedTLS one DER blob at a time.
bool LoadWindowsRoots(mbedtls_x509_crt* chain, base::String* error) {
  HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
  if (store == nullptr) {
    *error = "cannot open the windows ROOT certificate store";
    return false;
  }
  int loaded = 0;
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = ::CertEnumCertificatesInStore(store, cert)) != nullptr) {
    if (mbedtls_x509_crt_parse_der(chain, cert->pbCertEncoded,
                                   cert->cbCertEncoded) == 0)
      ++loaded;
  }
  ::CertCloseStore(store, 0);
  if (loaded == 0) {
    *error = "the windows ROOT store holds no certificate mbedtls can parse";
    return false;
  }
  return true;
}
#endif

// Fills `chain` with the certificates to trust. An empty `ca_file` means the
// system store. Failing here fails the request: a client that cannot build a
// trust store must not fall back to trusting everything.
bool LoadTrustStore(mbedtls_x509_crt* chain,
                    const base::String& ca_file,
                    base::String* error) {
  if (!ca_file.empty()) {
    const int rc = mbedtls_x509_crt_parse_file(chain, ca_file.c_str());
    if (rc != 0) {
      *error = MbedError("cannot read the ca bundle", rc);
      return false;
    }
    return true;
  }

  if (const char* env = std::getenv("SSL_CERT_FILE"); env != nullptr && *env != '\0') {
    const int rc = mbedtls_x509_crt_parse_file(chain, env);
    if (rc == 0)
      return true;
    RX_WARN("http: SSL_CERT_FILE={} is unreadable, falling back to the system paths", env);
  }

#ifdef _WIN32
  return LoadWindowsRoots(chain, error);
#else
  for (const char* path : kCaBundlePaths) {
    if (mbedtls_x509_crt_parse_file(chain, path) == 0)
      return true;
  }
  // A directory of hashed certs is the other common shape (openssl c_rehash).
  if (mbedtls_x509_crt_parse_path(chain, "/etc/ssl/certs") >= 0 && chain->version != 0)
    return true;
  *error =
      "no ca bundle found (looked at SSL_CERT_FILE and the usual /etc/ssl paths); "
      "set RX_HTTP_CA_FILE or Request::ca_file";
  return false;
#endif
}

class TlsStream final : public Stream {
 public:
  explicit TlsStream(const TlsOptions& options) : options_(options) {
    mbedtls_ssl_init(&ssl_);
    mbedtls_ssl_config_init(&conf_);
    mbedtls_x509_crt_init(&ca_);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_init(&entropy_);
    mbedtls_ctr_drbg_init(&drbg_);
#endif
  }

  ~TlsStream() override {
    if (handshaked_)
      mbedtls_ssl_close_notify(&ssl_);
    mbedtls_ssl_free(&ssl_);
    mbedtls_ssl_config_free(&conf_);
    mbedtls_x509_crt_free(&ca_);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_free(&drbg_);
    mbedtls_entropy_free(&entropy_);
#endif
  }

  bool Connect(const base::String& host,
               u16 port,
               u32 timeout_ms,
               base::String* error) override {
    EnsurePsaCrypto();

    inner_ = MakeTcpStream();
    if (!inner_->Connect(host, port, timeout_ms, error))
      return false;

#if MBEDTLS_VERSION_MAJOR < 4
    const char* personalization = "rx::http";
    const int seeded = mbedtls_ctr_drbg_seed(
        &drbg_, mbedtls_entropy_func, &entropy_,
        reinterpret_cast<const unsigned char*>(personalization),
        std::strlen(personalization));
    if (seeded != 0) {
      *error = MbedError("cannot seed the rng", seeded);
      return false;
    }
#endif

    if (options_.verify_peer && !LoadTrustStore(&ca_, options_.ca_file, error))
      return false;

    int rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
      *error = MbedError("cannot configure tls", rc);
      return false;
    }
    mbedtls_ssl_conf_authmode(&conf_, options_.verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                           : MBEDTLS_SSL_VERIFY_NONE);
    if (options_.verify_peer)
      mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
#endif

    rc = mbedtls_ssl_setup(&ssl_, &conf_);
    if (rc != 0) {
      *error = MbedError("cannot set up the tls session", rc);
      return false;
    }
    // Both the SNI extension and the name the certificate is checked against.
    rc = mbedtls_ssl_set_hostname(&ssl_, host.c_str());
    if (rc != 0) {
      *error = MbedError("cannot set the tls hostname", rc);
      return false;
    }
    mbedtls_ssl_set_bio(&ssl_, this, &TlsStream::BioSend, &TlsStream::BioRecv, nullptr);

    while ((rc = mbedtls_ssl_handshake(&ssl_)) != 0) {
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      // The inner stream's own message (timeout, reset) says more than the
      // generic mbedTLS code that wraps it.
      *error = inner_error_.empty() ? MbedError("tls handshake failed", rc) : inner_error_;
      return false;
    }

    if (options_.verify_peer) {
      const u32 flags = mbedtls_ssl_get_verify_result(&ssl_);
      if (flags != 0) {
        char info[512] = {};
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
        char buffer[640] = {};
        std::snprintf(buffer, sizeof(buffer), "certificate rejected for %s: %s",
                      host.c_str(), info);
        *error = base::String(buffer);
        return false;
      }
    }

    handshaked_ = true;
    return true;
  }

  bool Write(const void* data, u32 size, base::String* error) override {
    const unsigned char* cursor = static_cast<const unsigned char*>(data);
    u32 left = size;
    while (left > 0) {
      const int written = mbedtls_ssl_write(&ssl_, cursor, left);
      if (written > 0) {
        cursor += written;
        left -= static_cast<u32>(written);
        continue;
      }
      if (written == MBEDTLS_ERR_SSL_WANT_READ || written == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      *error = inner_error_.empty() ? MbedError("tls write failed", written) : inner_error_;
      return false;
    }
    return true;
  }

  i64 Read(void* data, u32 size, base::String* error) override {
    for (;;) {
      const int got = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(data), size);
      if (got >= 0)
        return got;
      if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      // A peer that closes the session is end of stream, not an error, whether
      // it sent close_notify or just hung up: a "body ends at close" response
      // lands here and the caller decides whether what it read is complete.
      if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || got == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;
      *error = inner_error_.empty() ? MbedError("tls read failed", got) : inner_error_;
      return -1;
    }
  }

 private:
  static int BioSend(void* ctx, const unsigned char* buf, size_t len) {
    auto* self = static_cast<TlsStream*>(ctx);
    self->inner_error_.clear();
    if (!self->inner_->Write(buf, static_cast<u32>(len), &self->inner_error_))
      return MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(len);
  }

  static int BioRecv(void* ctx, unsigned char* buf, size_t len) {
    auto* self = static_cast<TlsStream*>(ctx);
    self->inner_error_.clear();
    const i64 got = self->inner_->Read(buf, static_cast<u32>(len), &self->inner_error_);
    if (got < 0)
      return MBEDTLS_ERR_NET_RECV_FAILED;
    // mbedTLS turns a zero-byte read into MBEDTLS_ERR_SSL_CONN_EOF itself.
    return static_cast<int>(got);
  }

  TlsOptions options_;
  std::unique_ptr<Stream> inner_;
  base::String inner_error_;  // the socket's reason for the last BIO failure
  bool handshaked_ = false;
  mbedtls_ssl_context ssl_{};
  mbedtls_ssl_config conf_{};
  mbedtls_x509_crt ca_{};
#if MBEDTLS_VERSION_MAJOR < 4
  mbedtls_entropy_context entropy_{};
  mbedtls_ctr_drbg_context drbg_{};
#endif
};

}  // namespace

std::unique_ptr<Stream> MakeTlsStream(const TlsOptions& options) {
  return std::make_unique<TlsStream>(options);
}

}  // namespace rx::http

#else  // RX_HTTP_TLS

namespace rx::http {

std::unique_ptr<Stream> MakeTlsStream(const TlsOptions&) {
  return nullptr;
}

}  // namespace rx::http

#endif  // RX_HTTP_TLS
