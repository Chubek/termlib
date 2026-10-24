#include "openai/openai.h"
#include "common/ts_std_common.h"
#ifdef TS_STD_HTTP_TLS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <OpenAI.hpp>
#include <cstring>
#include <memory>

namespace {
namespace api = openapipp::low;
const char client_tag = 0;
using Client = std::unique_ptr<api::HttpClient>;
void destroy(void *p) { delete static_cast<Client *>(p); }
const char *env(const char *key, const char *fallback) {
  const char *value = std::getenv(key);
  return value && *value ? value : fallback;
}
bool header_ok(const char *s) {
  for (; *s; ++s)
    if (static_cast<unsigned char>(*s) < 32 || *s == 127)
      return false;
  return true;
}
TS_Status create(TS_VM *vm, void *, const TS_Value *a, size_t n, TS_Value *r,
                 TS_Error *e) {
  const char *key = env("OPENAI_API_KEY", "");
  const char *base = "https://api.openai.com/v1";
  int64_t timeout = 60;
  if (ts_std_argc(vm, n, 0, 3, e, "openai.new") ||
      (n > 0 && ts_std_str(a, &key, e, "openai.new")) ||
      (n > 1 && ts_std_str(a + 1, &base, e, "openai.new")) ||
      (n > 2 && ts_std_int(a + 2, &timeout, e, "openai.new")))
    return TS_ERR_INVAL;
  if (!header_ok(key) || !header_ok(base) || timeout < 1 || timeout > 600)
    return ts_std_inval(
        e, "openai.new: invalid header or timeout (1..600 seconds)");
  try {
    std::string url(base);
    if (url.find_first_of("@?# ") != std::string::npos)
      return ts_std_inval(e, "openai.new: base URL cannot contain credentials, "
                             "query, fragment or spaces");
    auto authority_start = url.find("://");
    if (authority_start != std::string::npos) {
      auto authority =
          url.substr(authority_start + 3, url.find('/', authority_start + 3) -
                                              (authority_start + 3));
      auto colon = authority.find(':');
      if (colon != std::string::npos &&
          (colon + 1 == authority.size() ||
           authority.find_first_not_of("0123456789", colon + 1) !=
               std::string::npos))
        return ts_std_inval(e, "openai.new: port must contain only decimal "
                               "digits; IPv6 is unsupported");
    }
    auto parsed = api::parse_base_url(url);
    if (parsed.scheme != "https" && parsed.host != "127.0.0.1" &&
        parsed.host != "localhost")
      return ts_std_inval(e, "openai.new: HTTP is allowed only on loopback");
    // Aggregate initialization bypasses upstream's getenv-to-string defaults.
    api::ClientConfig cfg{url, key, "", "", "Termscript/openai", int(timeout)};
    auto owner = std::make_unique<Client>(
        std::make_unique<api::HttpClient>(std::move(cfg)));
    if (ts_value_make_handle(r, owner.get(), destroy, "openai client",
                             &client_tag) != TS_OK)
      return ts_std_oom(e);
    owner.release();
    return TS_OK;
  } catch (const std::bad_alloc &) {
    return ts_std_oom(e);
  } catch (...) {
    return ts_std_inval(
        e, "openai.new: invalid or unsupported client configuration");
  }
}
TS_Status close_client(TS_VM *vm, void *, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e) {
  if (ts_std_argc(vm, n, 1, 1, e, "openai.close"))
    return TS_ERR_INVAL;
  auto *c =
      static_cast<Client *>(ts_std_handle(a, &client_tag, e, "openai.close"));
  if (!c)
    return TS_ERR_INVAL;
  c->reset();
  ts_std_ret_nil(r);
  return TS_OK;
}
TS_Status result(TS_Value *r, TS_Error *e, bool ok, int status,
                 const std::string &body, const std::string &error) {
  auto json = api::json_object({{"ok", ok ? "true" : "false"},
                                {"status", std::to_string(status)},
                                {"body", api::json_string(body)},
                                {"error", api::json_string(error)}});
  ts_dom_t *dom = nullptr;
  auto st = ts_dom_parse_json(json.c_str(), &dom, e);
  return st ? st : ts_dom_wrap(r, dom, e);
}
enum Operation {
  responses,
  chat,
  models,
  retrieve,
  cancel,
  remove_response,
  prompt
};
TS_Status request(TS_VM *vm, void *userdata, const TS_Value *a, size_t n,
                  TS_Value *r, TS_Error *e) {
  auto op = *static_cast<const Operation *>(userdata);
  size_t argc = op == models ? 1 : op == prompt ? 3 : 2;
  const char *arg = "", *input = "";
  if (ts_std_argc(vm, n, argc, argc, e, "openai.request"))
    return TS_ERR_INVAL;
  auto *c =
      static_cast<Client *>(ts_std_handle(a, &client_tag, e, "openai.request"));
  if (!c)
    return TS_ERR_INVAL;
  if (!*c)
    return ts_std_inval(e, "openai.request: client is closed");
  if ((argc > 1 && ts_std_str(a + 1, &arg, e, "openai.request")) ||
      (argc > 2 && ts_std_str(a + 2, &input, e, "openai.prompt")))
    return TS_ERR_INVAL;
  if (std::strlen(arg) > 1048576 || std::strlen(input) > 1048576) {
    ts_error_set(e, TS_ERR_LIMIT, 0, 0, "openai.request: input exceeds 1 MiB");
    return TS_ERR_LIMIT;
  }
  try {
    if (op == responses || op == chat) {
      ts_dom_t *raw = nullptr;
      auto st = ts_dom_parse_json(arg, &raw, e);
      if (st)
        return st;
      std::unique_ptr<ts_dom_t, decltype(&ts_dom_free)> dom(raw, ts_dom_free);
      if (std::strcmp(ts_dom_kind_str(raw), "object"))
        return ts_std_inval(e, "openai.request: expected JSON object");
      auto stream = ts_dom_key(raw, "stream");
      if (stream) {
        TS_Value value{};
        st = ts_dom_to_value(stream, &value, e);
        bool allowed = !st && value.type == TS_BOOL && !value.as.boolean;
        ts_value_free(&value);
        if (st)
          return st;
        if (!allowed)
          return ts_std_inval(e, "openai.request: streaming is not supported");
      }
    }
    std::string body;
    switch (op) {
    case responses:
      body = api::responses_create(**c, arg);
      break;
    case chat:
      body = api::chat_completions_create(**c, arg);
      break;
    case models:
      body = api::models_list(**c);
      break;
    case retrieve:
      body = api::responses_retrieve(**c, arg);
      break;
    case cancel:
      body = api::responses_cancel(**c, arg);
      break;
    case remove_response:
      body = api::responses_delete(**c, arg);
      break;
    case prompt:
      api::require_nonempty(arg, "model");
      body = api::responses_create(
          **c, api::json_object({{"model", api::json_string(arg)},
                                 {"input", api::json_string(input)},
                                 {"store", "false"}}));
      break;
    }
    // Upstream discards the exact successful HTTP status.
    return result(r, e, true, 0, body, "");
  } catch (const api::HttpError &ex) {
    try {
      return result(r, e, false, ex.http_status, ex.response_body, ex.what());
    } catch (const std::bad_alloc &) {
      return ts_std_oom(e);
    } catch (...) {
      return ts_std_inval(e, "openai: could not encode error result");
    }
  } catch (const std::bad_alloc &) {
    return ts_std_oom(e);
  } catch (const std::invalid_argument &) {
    return ts_std_inval(e, "openai: invalid request argument");
  } catch (...) {
    ts_error_set(e, TS_ERR_SYSTEM, 0, 0, "openai: unexpected client failure");
    return TS_ERR_SYSTEM;
  }
}
Operation operations[] = {responses,       chat,  models, retrieve, cancel,
                          remove_response, prompt};
const TS_FuncDef funcs[] = {{"new", create, nullptr},
                            {"close", close_client, nullptr},
                            {"responses", request, &operations[0]},
                            {"chat", request, &operations[1]},
                            {"models", request, &operations[2]},
                            {"retrieve", request, &operations[3]},
                            {"cancel", request, &operations[4]},
                            {"delete", request, &operations[5]},
                            {"prompt", request, &operations[6]}};
} // namespace
const TS_Module ts_std_openai_module = TS_MODULE_INIT("std.openai", funcs);
