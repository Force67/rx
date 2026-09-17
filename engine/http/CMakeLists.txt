# rx::http: a blocking HTTP/1.1 client, with https on top of mbedTLS.
#
# Small on purpose. The engine needs to call a web API now and then (a server
# list, an update check) and that should not cost a dependency the size of
# curl; http.h documents what this deliberately does not do. Everything except
# the TLS record layer is hand-written, so a build without an mbedTLS target
# still links and plain http still works.
rx_add_module(http
  url.cc
  http.cc
  tcp_stream.cc
  tls_stream.cc
)
target_link_libraries(rx_http PUBLIC rx::core)

# The mbedtls target comes from the top-level CMakeLists (or from a parent
# project that already builds one). Absent, MakeTlsStream returns null and
# every https URL fails with a message saying exactly that.
if(RX_HTTPS AND TARGET mbedtls)
  target_link_libraries(rx_http PRIVATE mbedtls)
  target_compile_definitions(rx_http PRIVATE RX_HTTP_TLS=1)
  if(WIN32)
    # Windows keeps its roots in the "ROOT" certificate store rather than in a
    # bundle on disk, and crypt32 is how they are enumerated.
    target_link_libraries(rx_http PRIVATE crypt32)
  endif()
else()
  message(STATUS "rx::http: no mbedtls target; https requests will fail")
endif()

if(WIN32)
  target_link_libraries(rx_http PRIVATE ws2_32)
endif()
