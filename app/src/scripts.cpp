#include "scripts.h"

#include <cctype>
#include <chrono>
#include <memory>
#include <thread>

#include "util/exec.h"
#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb {

namespace {

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string cmdline(const std::string& script, const std::vector<std::string>& args) {
  std::string s = script;
  for (const std::string& a : args) s += " " + a;
  return s;
}

}  // namespace

bool parse_script_json(const std::string& out, json* j, std::string* err) {
  const std::string t = trim(out);
  if (t.empty()) {
    *err = "printed nothing";
    return false;
  }
  try {
    *j = json::parse(t);
  } catch (const std::exception&) {
    // The first line is what a person would recognise: a shell error, a usage message.
    const std::string first = trim(split(t, '\n').empty() ? t : split(t, '\n')[0]);
    *err = "printed something that is not JSON: " + first.substr(0, 120);
    return false;
  }
  if (!j->is_object() && !j->is_array()) {
    *err = "printed a JSON " + std::string(j->type_name()) + ", not an object or array";
    return false;
  }
  return true;
}

bool audio_mode_ok(const std::string& m) { return m == "pipewire" || m == "bluealsa" || m == "none"; }

bool wifi_mode_ok(const std::string& m) { return m == "auto" || m == "sta" || m == "ap" || m == "off"; }

bool wifi_ssid_ok(const std::string& ssid, std::string* err) {
  if (ssid.empty() || ssid.size() > 32) {
    *err = "an SSID is 1 to 32 bytes";
    return false;
  }
  for (unsigned char c : ssid) {
    // Any byte is a legal SSID, but a control character or a newline would corrupt the network
    // file the script writes; nobody names their network with one.
    if (c < 0x20 || c == 0x7f) {
      *err = "the SSID has a control character";
      return false;
    }
  }
  return true;
}

bool wifi_psk_ok(const std::string& psk, std::string* err) {
  if (psk.empty()) return true;  // an open network
  if (psk.size() == 64) {
    for (char c : psk)
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        *err = "a 64-character key is hex (a raw PSK)";
        return false;
      }
    return true;
  }
  if (psk.size() < 8 || psk.size() > 63) {
    *err = "a WPA passphrase is 8 to 63 characters";
    return false;
  }
  for (unsigned char c : psk)
    if (c < 0x20 || c == 0x7f) {
      *err = "the passphrase has a control character";
      return false;
    }
  return true;
}

bool TargetScripts::available(const std::string& script) const { return !find_exec(script, dirs_).empty(); }

std::string TargetScripts::missing_reason(const std::string& script) {
  return script + " is not installed here: it is one of the bench's target scripts, on the board's "
                  "image (running on a PC, this part of the API has nothing to drive)";
}

TargetScripts::Result TargetScripts::json_cmd(const std::string& script, const std::vector<std::string>& args,
                                              int timeout_ms) const {
  const std::string path = find_exec(script, dirs_);
  if (path.empty()) return {503, json{{"error", missing_reason(script)}}};
  std::vector<std::string> argv{path};
  argv.insert(argv.end(), args.begin(), args.end());
  const ExecResult r = run_cmd(argv, timeout_ms);
  if (r.timed_out) return {504, json{{"error", cmdline(script, args) + ": timed out"}}};
  if (!r.ok()) return {500, json{{"error", cmdline(script, args) + ": " + r.reason()}}};
  json j;
  std::string err;
  if (!parse_script_json(r.out, &j, &err)) return {500, json{{"error", cmdline(script, args) + " " + err}}};
  return {200, j};
}

TargetScripts::Result TargetScripts::action(const std::string& script, const std::vector<std::string>& args,
                                            int timeout_ms) const {
  const std::string path = find_exec(script, dirs_);
  if (path.empty()) return {503, json{{"error", missing_reason(script)}}};
  std::vector<std::string> argv{path};
  argv.insert(argv.end(), args.begin(), args.end());
  LOG_INFO("scripts: {}", cmdline(script, args));
  const ExecResult r = run_cmd(argv, timeout_ms);
  if (r.timed_out) return {504, json{{"error", cmdline(script, args) + ": timed out"}}};
  if (!r.ok()) {
    LOG_WARN("scripts: {} failed: {}", cmdline(script, args), r.reason());
    return {500, json{{"error", r.reason()}}};
  }
  return {200, json{{"ok", true}}};
}

TargetScripts::Result TargetScripts::background(const std::string& script, const std::vector<std::string>& args,
                                                int timeout_ms, int delay_ms) {
  const std::string path = find_exec(script, dirs_);
  if (path.empty()) return {503, json{{"error", missing_reason(script)}}};
  std::mutex* serial;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto& s = serial_[script];
    if (!s) s = std::make_unique<std::mutex>();
    serial = s.get();
    Background& b = bg_[script];
    b.op = trim(cmdline("", args));
    b.running = true;
    b.ok = false;
    b.error.clear();
    b.started_ms = wall_ms();
    b.finished_ms = 0;
  }
  std::vector<std::string> argv{path};
  argv.insert(argv.end(), args.begin(), args.end());
  std::thread([this, script, argv, timeout_ms, delay_ms, serial] {
    if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    std::lock_guard<std::mutex> one(*serial);
    LOG_INFO("scripts: {} (in the background)", cmdline(script, {argv.begin() + 1, argv.end()}));
    const ExecResult r = run_cmd(argv, timeout_ms);
    std::lock_guard<std::mutex> lk(m_);
    Background& b = bg_[script];
    b.running = false;
    b.ok = r.ok();
    b.error = r.ok() ? std::string{} : r.reason();
    b.finished_ms = wall_ms();
    if (!r.ok()) LOG_WARN("scripts: {} {} failed: {}", script, b.op, b.error);
  }).detach();
  return {202, json{{"ok", true}, {"pending", true}}};
}

json TargetScripts::background_json(const std::string& script) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = bg_.find(script);
  if (it == bg_.end()) return nullptr;
  const Background& b = it->second;
  return json{{"op", b.op},
              {"running", b.running},
              {"ok", b.ok},
              {"error", b.error},
              {"started_ms", b.started_ms},
              {"finished_ms", b.finished_ms ? json(b.finished_ms) : json(nullptr)}};
}

}  // namespace btb
