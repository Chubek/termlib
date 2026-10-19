/* Bounded, single-request HTTP client. Policy (navigation, redirects, UI)
   belongs to Termscript applications, not this transport adapter. */
#include "common/module_call.h"
#include "common/ts_std_common.h"
#include "http/http.h"
#include "map/map.h"
#include <lexbor/url/url.h>

#ifdef TS_STD_HTTP_TLS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "../../../third_party/cpp-httplib/httplib.h"
#include <cstring>
#include <new>

namespace {
constexpr size_t max_url = 8192;
constexpr size_t max_body = 1024 * 1024;

struct UrlParser {
  lxb_url_parser_t parser{};
  ~UrlParser() {
    lxb_url_parser_memory_destroy(&parser);
    lxb_url_parser_destroy(&parser, false);
  }
};

lxb_status_t append_url(const lxb_char_t *s, size_t n, void *p) {
  auto *b = static_cast<ts_sbuf_t *>(p);
  if (n > max_url - b->len)
    return LXB_STATUS_ERROR_OVERFLOW;
  return ts_sbuf_put(b, reinterpret_cast<const char *>(s), n)
             ? LXB_STATUS_ERROR_MEMORY_ALLOCATION
             : LXB_STATUS_OK;
}

bool input_ok(const char *s) {
  if (std::strlen(s) > max_url)
    return false;
  for (; *s; ++s)
    if (static_cast<unsigned char>(*s) < 32 || *s == 127)
      return false;
  return true;
}

/* Always frees parser arena and serialization buffer, including parse errors.
 */
TS_Status resolve(const char *base, const char *relative, TS_Value *r,
                  TS_Error *e) {
  ts_std_ret_nil(r);
  if (!input_ok(base) || !input_ok(relative))
    return TS_OK;
  UrlParser owner;
  auto &parser = owner.parser;
  /* Own the arena explicitly so even a partially initialized arena is freed. */
  parser.mraw = lexbor_mraw_create();
  if (!parser.mraw || lexbor_mraw_init(parser.mraw, 4096) != LXB_STATUS_OK)
    return ts_std_oom(e);
  if (lxb_url_parser_init(&parser, parser.mraw) != LXB_STATUS_OK)
    return ts_std_oom(e);
  lxb_url_t *parent = nullptr;
  if (*base) {
    parent = lxb_url_parse(&parser, nullptr,
                           reinterpret_cast<const lxb_char_t *>(base),
                           std::strlen(base));
    if (!parent)
      return TS_OK;
    lxb_url_parser_clean(&parser);
  }
  auto *url = lxb_url_parse(&parser, parent,
                            reinterpret_cast<const lxb_char_t *>(relative),
                            std::strlen(relative));
  if (!url ||
      (url->scheme.type != LXB_URL_SCHEMEL_TYPE_HTTP &&
       url->scheme.type != LXB_URL_SCHEMEL_TYPE_HTTPS) ||
      url->username.length || url->password.length)
    return TS_OK;
  ts_sbuf_t buffer{};
  auto status = lxb_url_serialize(url, append_url, &buffer, false);
  if (status != LXB_STATUS_OK) {
    ts_sbuf_free(&buffer);
    if (status == LXB_STATUS_ERROR_MEMORY_ALLOCATION)
      return ts_std_oom(e);
    return TS_OK;
  }
  return ts_std_ret_sbuf(r, &buffer, e);
}

TS_Status result(TS_VM *vm, TS_Value *r, TS_Error *e, bool ok, int status,
                 const std::string &body, const std::string &location,
                 const std::string &type, const char *message) {
  TS_Status st =
      ts_module_call(vm, &ts_std_map_module, "new", nullptr, 0, r, e);
  const char *keys[] = {"ok",       "status",       "body",
                        "location", "content_type", "error"};
  TS_Value values[] = {ts_arg_int(0),
                       ts_arg_int(status),
                       ts_arg_str(body.c_str()),
                       ts_arg_str(location.c_str()),
                       ts_arg_str(type.c_str()),
                       ts_arg_str(message)};
  values[0].type = TS_BOOL;
  values[0].as.boolean = ok;
  for (size_t i = 0; !st && i < 6; ++i) {
    TS_Value args[] = {*r, ts_arg_str(keys[i]), values[i]}, ignored{};
    st = ts_module_call(vm, &ts_std_map_module, "set", args, 3, &ignored, e);
    ts_value_free(&ignored);
  }
  if (st)
    ts_value_free(r);
  return st;
}
} // namespace

extern "C" TS_Status ts_http_resolve(TS_VM *vm, void *, const TS_Value *a,
                                     size_t n, TS_Value *r, TS_Error *e) {
  const char *base, *relative;
  if (ts_std_argc(vm, n, 2, 2, e, "http.resolve") ||
      ts_std_str(a, &base, e, "http.resolve") ||
      ts_std_str(a + 1, &relative, e, "http.resolve"))
    return TS_ERR_INVAL;
  return resolve(base, relative, r, e);
}

extern "C" TS_Status ts_http_fetch(TS_VM *vm, void *, const TS_Value *a,
                                   size_t n, TS_Value *r, TS_Error *e) {
  const char *source;
  int64_t timeout = 10000, limit = max_body;
  if (ts_std_argc(vm, n, 1, 3, e, "http.fetch") ||
      ts_std_str(a, &source, e, "http.fetch") ||
      (n > 1 && ts_std_int(a + 1, &timeout, e, "http.fetch")) ||
      (n > 2 && ts_std_int(a + 2, &limit, e, "http.fetch")))
    return TS_ERR_INVAL;
  if (timeout < 1 || timeout > 60000 || limit < 0 || limit > int64_t(max_body))
    return ts_std_inval(e, "http.fetch: timeout 1..60000 ms; limit 0..1048576");
  TS_Value normalized{};
  auto st = resolve("", source, &normalized, e);
  if (st)
    return st;
  if (normalized.type != TS_STRING)
    return result(vm, r, e, false, 0, "", "", "", "invalid or unsupported URL");
  /* Copy before releasing normalized, even on allocation failure. */
  std::string url;
  try {
    url = normalized.as.string;
  } catch (const std::bad_alloc &) {
    ts_value_free(&normalized);
    return ts_std_oom(e);
  }
  ts_value_free(&normalized);
  try {
#ifndef TS_STD_HTTP_TLS
    if (url.compare(0, 8, "https://") == 0)
      return result(vm, r, e, false, 0, "", "", "",
                    "HTTPS support disabled at build time");
#endif
    url.erase(url.find('#') == std::string::npos ? url.size() : url.find('#'));
    auto path_at = url.find('/', url.find("://") + 3);
    std::string origin = url.substr(0, path_at);
    std::string path = path_at == std::string::npos ? "/" : url.substr(path_at);
    httplib::Client client(origin);
    auto duration = std::chrono::milliseconds(timeout);
    client.set_connection_timeout(duration);
    client.set_read_timeout(duration);
    client.set_write_timeout(duration);
    client.set_max_timeout(duration);
    client.set_follow_location(false);
    client.set_keep_alive(false);
    client.set_decompress(false);
    client.set_path_encode(false); // Lexbor already percent-encoded it.
#ifdef TS_STD_HTTP_TLS
    client.enable_server_certificate_verification(true);
    client.enable_server_hostname_verification(true);
#endif
    std::string body;
    const char *failure = nullptr;
    auto response = client.Get(
        path,
        httplib::Headers{{"User-Agent", "xw3m/0.1 Termscript"},
                         {"Accept", "text/html, text/plain"},
                         {"Accept-Encoding", "identity"}},
        [&](const httplib::Response &headers) {
          auto encoding = headers.get_header_value("Content-Encoding");
          if (!encoding.empty() && encoding != "identity") {
            failure = "unsupported content encoding";
            return false;
          }
          return true;
        },
        [&](const char *data, size_t length) {
          if (length > size_t(limit) - body.size()) {
            failure = "response body exceeds limit";
            return false;
          }
          if (std::memchr(data, 0, length)) {
            failure = "binary response is not supported";
            return false;
          }
          body.append(data, length);
          return true;
        });
    if (!response)
      return result(vm, r, e, false, 0, "", "", "",
                    failure ? failure
                            : httplib::to_string(response.error()).c_str());
    return result(vm, r, e, true, response->status, body,
                  response->get_header_value("Location"),
                  response->get_header_value("Content-Type"), "");
  } catch (const std::bad_alloc &) {
    return ts_std_oom(e);
  } catch (const std::exception &) {
    return result(vm, r, e, false, 0, "", "", "", "HTTP client failure");
  }
}
