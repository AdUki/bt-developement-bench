#pragma once

// What every route file shares: the JSON reply helpers and the scaffold of a JSON-bodied handler.

#include <httplib.h>

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

namespace btb {

inline void send_json(httplib::Response& res, const nlohmann::json& j, int status = 200) {
  res.status = status;
  // replace: text from a device (a name, a GATT string) is attacker-controlled bytes, and invalid
  // UTF-8 in it must cost a replacement character, not a 500.
  res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
}

inline void send_error(httplib::Response& res, int status, const std::string& msg) {
  send_json(res, nlohmann::json{{"error", msg}}, status);
}

inline void send_ok(httplib::Response& res) { send_json(res, nlohmann::json{{"ok", true}}); }

// Parse the body, turn a parse/type error into a 400, and answer {"ok":true} unless the handler
// already sent its own reply. An empty body is {}.
template <class Fn>
auto json_route(Fn fn) {
  return [fn](const httplib::Request& req, httplib::Response& res) {
    try {
      const nlohmann::json j = req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
      fn(j, req, res);
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    if (res.body.empty()) send_ok(res);
  };
}

inline std::string path_param(const httplib::Request& req, const char* name) {
  const auto it = req.path_params.find(name);
  return it == req.path_params.end() ? std::string{} : it->second;
}

inline std::string query(const httplib::Request& req, const char* key, const std::string& fallback = "") {
  return req.has_param(key) ? req.get_param_value(key) : fallback;
}

inline long query_long(const httplib::Request& req, const char* key, long fallback) {
  if (!req.has_param(key)) return fallback;
  return std::strtol(req.get_param_value(key).c_str(), nullptr, 10);
}

}  // namespace btb
