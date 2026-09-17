// rx::http acceptance: URL parsing and redirect resolution, response-head
// parsing, chunked decoding, and the refusals that keep a hostile string from
// becoming a second request. All pure parsing, no socket involved, so plain
// `ctest` runs it everywhere.
//
// The live half (a real connect, a real handshake) cannot run in ctest without
// reaching the network; RX_HTTP_LIVE=<url> drives it by hand:
//   RX_HTTP_LIVE=https://example.com ./build/linux/http_test

#include <cstdio>
#include <cstdlib>

#include <base/option.h>

#include "http/http.h"
#include "http/url.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace http = rx::http;
using rx::u16;
using rx::u32;

void TestUrlParse() {
  http::Url url;

  CHECK(http::Url::Parse("http://example.com", &url));
  CHECK_EQ(url.scheme, base::String("http"));
  CHECK_EQ(url.host, base::String("example.com"));
  CHECK_EQ(url.port, u16{80});
  CHECK_EQ(url.target, base::String("/"));
  CHECK(!url.tls);

  CHECK(http::Url::Parse("HTTPS://Example.com/v1/servers?domain=skyrim", &url));
  CHECK_EQ(url.scheme, base::String("https"));
  CHECK_EQ(url.port, u16{443});
  CHECK(url.tls);
  CHECK_EQ(url.target, base::String("/v1/servers?domain=skyrim"));

  CHECK(http::Url::Parse("http://127.0.0.1:8477/v1/stats", &url));
  CHECK_EQ(url.port, u16{8477});
  CHECK_EQ(url.Authority(), base::String("127.0.0.1:8477"));

  // The default port is left off the Host header, a non-default one is not.
  CHECK(http::Url::Parse("https://list.example:443/x", &url));
  CHECK_EQ(url.Authority(), base::String("list.example"));

  // An IPv6 literal must be bracketed, and comes back bracketed.
  CHECK(http::Url::Parse("http://[2001:db8::1]:8477/", &url));
  CHECK_EQ(url.host, base::String("2001:db8::1"));
  CHECK_EQ(url.port, u16{8477});
  CHECK_EQ(url.Authority(), base::String("[2001:db8::1]:8477"));

  // A query with no path still sends a valid request line.
  CHECK(http::Url::Parse("http://example.com?q=1", &url));
  CHECK_EQ(url.target, base::String("/?q=1"));

  // The fragment never reaches the wire.
  CHECK(http::Url::Parse("http://example.com/a#section", &url));
  CHECK_EQ(url.target, base::String("/a"));

  CHECK(!http::Url::Parse("example.com/x", &url));          // no scheme
  CHECK(!http::Url::Parse("ftp://example.com/x", &url));    // not ours to speak
  CHECK(!http::Url::Parse("http://", &url));                // no host
  CHECK(!http::Url::Parse("http://u:p@example.com", &url)); // credentials refused
  CHECK(!http::Url::Parse("http://example.com:0/", &url));  // port 0 is not dialable
  CHECK(!http::Url::Parse("http://example.com:99999/", &url));
  CHECK(!http::Url::Parse("http://2001:db8::1/", &url));    // unbracketed ipv6
  CHECK(!http::Url::Parse("http://example.com/a\r\nX: y", &url));  // request splitting
}

void TestUrlResolve() {
  http::Url base_url;
  CHECK(http::Url::Parse("https://list.example/v1/servers?x=1", &base_url));

  http::Url next;
  CHECK(base_url.Resolve("/v2/servers", &next));
  CHECK_EQ(next.host, base::String("list.example"));
  CHECK_EQ(next.target, base::String("/v2/servers"));
  CHECK(next.tls);

  CHECK(base_url.Resolve("http://other.example/x", &next));
  CHECK_EQ(next.host, base::String("other.example"));
  CHECK(!next.tls);

  // Relative: the last path segment is replaced, the query dropped.
  CHECK(base_url.Resolve("stats", &next));
  CHECK_EQ(next.target, base::String("/v1/stats"));

  CHECK(!base_url.Resolve("", &next));
  CHECK(!base_url.Resolve("gopher://example.com/x", &next));
}

void TestResponseHead() {
  http::Response response;
  const base::String head =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 27\r\n"
      "X-Odd\r\n"  // junk without a colon: skipped, not fatal
      "Server: axum\r\n";
  CHECK(http::ParseResponseHead(head, &response));
  CHECK_EQ(response.status, u16{200});
  CHECK(response.ok());
  CHECK_EQ(response.headers.size(), size_t{3});

  // Lookup is case-insensitive, and the value is trimmed.
  const base::String* type = response.Find("CONTENT-TYPE");
  CHECK(type != nullptr && *type == base::String("application/json"));
  CHECK(response.Find("nothing-here") == nullptr);

  http::Response created;
  CHECK(http::ParseResponseHead("HTTP/1.1 201 Created\r\nLocation: /v1/x\r\n", &created));
  CHECK_EQ(created.status, u16{201});
  CHECK(created.ok());  // the whole 2xx range counts, not just 200

  http::Response minimal;
  CHECK(http::ParseResponseHead("HTTP/1.0 204 ", &minimal));
  CHECK_EQ(minimal.status, u16{204});

  http::Response bad;
  CHECK(!http::ParseResponseHead("220 smtp.example ESMTP", &bad));
  CHECK(!http::ParseResponseHead("HTTP/1.1 99 Nope\r\n", &bad));
  CHECK(!http::ParseResponseHead("HTTP/1.1 twenty\r\n", &bad));
}

void TestChunked() {
  base::String body;
  CHECK(http::DecodeChunked("4\r\nrx::\r\n4\r\nhttp\r\n0\r\n\r\n", &body, 1024));
  CHECK_EQ(body, base::String("rx::http"));

  // Chunk extensions are ignored, trailers are skipped.
  CHECK(http::DecodeChunked("5;x=1\r\nhello\r\n0\r\nX-Sum: 3\r\n\r\n", &body, 1024));
  CHECK_EQ(body, base::String("hello"));

  CHECK(http::DecodeChunked("0\r\n\r\n", &body, 1024));
  CHECK(body.empty());

  // Incomplete is not done: the client reads more rather than truncating.
  CHECK(!http::DecodeChunked("4\r\nrx::\r\n", &body, 1024));
  CHECK(!http::DecodeChunked("4\r\nrx", &body, 1024));
  // Malformed and over-cap both fail.
  CHECK(!http::DecodeChunked("zz\r\nrx\r\n0\r\n\r\n", &body, 1024));
  CHECK(!http::DecodeChunked("8\r\nrx::http\r\n0\r\n\r\n", &body, 4));
}

void TestRequestRefusals() {
  // Bad input is refused before a socket is opened, so these are instant and
  // need no network: status stays 0 and the reason is in `error`.
  http::Request request;
  request.url = "http://example.com/a";
  request.headers.push_back(http::Header{"X-Evil", "a\r\nHost: elsewhere"});
  const http::Response injected = http::Fetch(request);
  CHECK_EQ(injected.status, u16{0});
  CHECK(!injected.error.empty());

  const http::Response unparsable = http::Get("not a url");
  CHECK_EQ(unparsable.status, u16{0});
  CHECK(!unparsable.error.empty());

  // An https URL on a build without a TLS backend must fail, never downgrade.
  if (!http::TlsAvailable()) {
    const http::Response no_tls = http::Get("https://example.com/");
    CHECK_EQ(no_tls.status, u16{0});
    CHECK(!no_tls.error.empty());
  }
}

// Off by default: reaches the network, so it only runs when asked.
void TestLive() {
  const char* target = std::getenv("RX_HTTP_LIVE");
  if (target == nullptr || *target == '\0')
    return;
  std::printf("http_test: live fetch of %s (tls backend: %s)\n", target,
              http::TlsAvailable() ? "yes" : "no");
  const http::Response response = http::Get(target);
  std::printf("  status %u, %zu bytes, error '%s'\n", unsigned(response.status),
              size_t(response.body.size()), response.error.c_str());
  // The first line or so of the body, which is how a probe endpoint (say
  // howsmyssl) is read back: the answer is the point of the fetch. The whole
  // body goes to RX_HTTP_LIVE_OUT when the interesting part is further in.
  if (!response.body.empty()) {
    const size_t shown = response.body.size() < 240 ? response.body.size() : 240;
    std::printf("  body: %.*s\n", int(shown), response.body.c_str());
    if (const char* out = std::getenv("RX_HTTP_LIVE_OUT"); out != nullptr && *out != '\0') {
      if (std::FILE* file = std::fopen(out, "wb"); file != nullptr) {
        std::fwrite(response.body.c_str(), 1, response.body.size(), file);
        std::fclose(file);
        std::printf("  body written to %s\n", out);
      }
    }
  }
  CHECK(response.ok());
  CHECK(!response.body.empty());
}

}  // namespace

int main() {
  // The client's own knobs (RX_HTTP_TLS_DEBUG) are base::Options, and nothing
  // else in a bare test binary populates them from the environment.
  base::InitOptionsFromEnv();
  TestUrlParse();
  TestUrlResolve();
  TestResponseHead();
  TestChunked();
  TestRequestRefusals();
  TestLive();
  if (g_failures == 0) {
    std::printf("http_test: all passed\n");
    return 0;
  }
  std::printf("http_test: %d failure(s)\n", g_failures);
  return 1;
}
