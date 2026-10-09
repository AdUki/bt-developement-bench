#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb {

// The target scripts (docs/contracts.md: btbench-audio, btbench-wifi, btbench-kernel) print JSON
// on stdout for status/scan, and fail with a non-zero exit and one line on stderr. Parsed here,
// strictly: a script that prints something else is a broken script, reported as such.
bool parse_script_json(const std::string& out, nlohmann::json* j, std::string* err);

// Checks on what the API passes to the scripts as arguments: they reach a shell script, so they
// are refused here rather than quoted there.
bool audio_mode_ok(const std::string& m);
bool wifi_mode_ok(const std::string& m);
bool wifi_ssid_ok(const std::string& ssid, std::string* err);
bool wifi_psk_ok(const std::string& psk, std::string* err);

// Runs the scripts. On a PC they are not installed, which every caller turns into a 503 with
// `missing_reason()`.
class TargetScripts {
 public:
  struct Result {
    int http = 200;  // 200, 503 (not installed), 500 (failed), 504 (timed out)
    nlohmann::json body;
  };
  // An operation that runs on after the HTTP request has been answered: Wi-Fi changes (which can
  // take the client's own connection down) and a kernel trial (which reboots).
  struct Background {
    std::string op;  // "add home-ap", "mode ap", "try"
    bool running = false;
    bool ok = false;
    std::string error;
    int64_t started_ms = 0;
    int64_t finished_ms = 0;
  };

  explicit TargetScripts(std::string extra_dirs = "") : dirs_(std::move(extra_dirs)) {}

  bool available(const std::string& script) const;
  static std::string missing_reason(const std::string& script);

  // stdout as JSON.
  Result json_cmd(const std::string& script, const std::vector<std::string>& args, int timeout_ms) const;
  // An action: {"ok":true} on success.
  Result action(const std::string& script, const std::vector<std::string>& args, int timeout_ms) const;
  // Answers at once ({"ok":true,"pending":true}) and runs it on a thread of its own, one at a time
  // per script; background_json() says how it went. `delay_ms` lets the HTTP reply leave first.
  Result background(const std::string& script, const std::vector<std::string>& args, int timeout_ms,
                    int delay_ms = 0);
  nlohmann::json background_json(const std::string& script) const;

 private:
  std::string dirs_;
  mutable std::mutex m_;
  std::map<std::string, Background> bg_;
  std::map<std::string, std::unique_ptr<std::mutex>> serial_;
};

}  // namespace btb
