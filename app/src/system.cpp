#include "system.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "util/exec.h"
#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb {

namespace {

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

json env_json(const std::string& path) {
  std::ifstream f(path);
  if (!f) return nullptr;
  json j = json::object();
  for (const auto& kv : parse_env(read_file(path))) j[kv.first] = kv.second;
  return j;
}

}  // namespace

const std::vector<std::string>& watched_services() {
  static const std::vector<std::string> s = {
      "bluetooth.service",       "pipewire.service",      "wireplumber.service",
      "bluealsa.service",        "btbench-btsnoop.service", "btbench-wifi.service",
      "btbenchd.service"};
  return s;
}

bool restartable_service(const std::string& unit) {
  for (const std::string& s : watched_services())
    if (unit == s && unit != "btbenchd.service") return true;
  return false;
}

json parse_is_active(const std::vector<std::string>& units, const std::string& out) {
  const std::vector<std::string> lines = split(out, '\n', false);
  json j = json::object();
  for (size_t i = 0; i < units.size(); ++i) {
    const std::string s = i < lines.size() ? trim(lines[i]) : std::string{};
    j[units[i]] = s.empty() ? "unknown" : s;
  }
  return j;
}

std::string parse_version(const std::string& out) {
  // The first thing that looks like a version number: digits, a dot, digits.
  for (size_t i = 0; i < out.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(out[i]))) continue;
    // Not the middle of a word or of a number already passed ("libpipewire" is a word, "v4.3" is not).
    const char prev = i > 0 ? out[i - 1] : ' ';
    if (prev == '.' || (std::isalnum(static_cast<unsigned char>(prev)) && prev != 'v' && prev != 'V')) continue;
    size_t j = i;
    bool dot = false;
    while (j < out.size() && (std::isdigit(static_cast<unsigned char>(out[j])) || out[j] == '.')) {
      if (out[j] == '.') dot = true;
      ++j;
    }
    if (!dot) continue;
    std::string v = out.substr(i, j - i);
    while (!v.empty() && v.back() == '.') v.pop_back();
    // A suffix that is part of the version (5.84-dirty, 1.4.2-12-gdeadbee) is kept.
    while (j < out.size() && (std::isalnum(static_cast<unsigned char>(out[j])) || out[j] == '-' ||
                              out[j] == '+' || out[j] == '~')) {
      v += out[j++];
    }
    return v;
  }
  return trim(split(out, '\n').empty() ? out : split(out, '\n')[0]);
}

bool bluetoothd_args_ok(const std::string& args, std::string* err) {
  if (args.size() > 512) {
    *err = "at most 512 characters";
    return false;
  }
  for (unsigned char c : args) {
    // The value lands in a systemd EnvironmentFile: no newlines, no quotes or expansions that
    // would make it mean something other than what was typed.
    if (c < 0x20 || c == '"' || c == '\\' || c == '$' || c == '`') {
      *err = "only options and their values (no quotes, $, \\ or control characters)";
      return false;
    }
  }
  // A bare word is fine as an option's value ("--plugin a2dp"), but not first: then it is no option.
  const std::vector<std::string> words = split_args(args);
  if (!words.empty() && words[0][0] != '-') {
    *err = "arguments start with an option, e.g. -d -E";
    return false;
  }
  return true;
}

std::string read_bluetoothd_args(const std::string& data_dir) {
  const auto env = parse_env(read_file(data_dir + "/bluetoothd.env"));
  const auto it = env.find("BLUETOOTHD_ARGS");
  return it == env.end() ? std::string{} : it->second;
}

bool write_bluetoothd_args(const std::string& data_dir, const std::string& args, std::string* err) {
  mkdir(data_dir.c_str(), 0755);
  const std::string path = data_dir + "/bluetoothd.env";
  const std::string tmp = path + ".new";
  // Only BLUETOOTHD_ARGS is ours: the file also holds BLUETOOTHD_NOPLUGIN (which keeps BlueZ's own
  // HFP plugin out of the audio stack's way) and whatever else the image put there, all kept.
  std::string out;
  bool replaced = false;
  std::istringstream in(read_file(path));
  for (std::string line; std::getline(in, line);) {
    std::string t = trim(line);
    if (starts_with(t, "export ")) t = trim(t.substr(7));
    if (starts_with(t, "BLUETOOTHD_ARGS=")) {
      if (!replaced) out += "BLUETOOTHD_ARGS=" + env_quote(args) + "\n";
      replaced = true;
      continue;
    }
    out += line + "\n";
  }
  if (!replaced) {
    if (out.empty()) out = "# bluetoothd's arguments, read by bluetooth.service (btbenchd: PUT /api/system/bluetoothd-args).\n";
    out += "BLUETOOTHD_ARGS=" + env_quote(args) + "\n";
  }
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      *err = "cannot write " + tmp;
      return false;
    }
    f << out;
    f.flush();
    if (!f) {
      *err = "cannot write " + tmp;
      return false;
    }
  }
  // Renamed into place: bluetooth.service starting at the wrong moment reads the old file or the
  // new one, never half of one.
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    *err = "cannot replace " + path;
    return false;
  }
  sync();
  return true;
}

std::string read_bluetoothd_noplugin(const std::string& data_dir) {
  const auto env = parse_env(read_file(data_dir + "/bluetoothd.env"));
  const auto it = env.find("BLUETOOTHD_NOPLUGIN");
  return it == env.end() ? std::string{} : it->second;
}

json sysinfo_json(const SysInfo& s) {
  return json{{"hostname", s.hostname},
              {"ips", s.ips},
              {"uptime_s", s.uptime_s},
              {"load1", s.load1},
              {"cpu_pct", s.cpu_pct},
              {"temp_c", s.temp_c >= 0 ? json(s.temp_c) : json(nullptr)},
              {"mem", {{"total_kb", s.mem.total_kb}, {"used_kb", s.mem.used_kb}, {"available_kb", s.mem.available_kb}}},
              {"power",
               {{"available", s.power.available},
                {"under_voltage", s.power.under_voltage},
                {"seen", s.power.seen}}}};
}

void SystemInfo::store_sample(const SysInfo& s) {
  std::lock_guard<std::mutex> lk(m_);
  sample_ = s;
}

SysInfo SystemInfo::last_sample() const {
  std::lock_guard<std::mutex> lk(m_);
  return sample_;
}

json SystemInfo::health() const { return sysinfo_json(last_sample()); }

json SystemInfo::services() const {
  std::vector<std::string> argv{"systemctl", "is-active"};
  for (const std::string& s : watched_services()) argv.push_back(s);
  // is-active exits non-zero when any unit is not active; the lines are what count.
  const ExecResult r = run_cmd(argv, 5000);
  if (!r.started) {
    json j = json::object();
    for (const std::string& s : watched_services()) j[s] = "unknown";
    return j;
  }
  return parse_is_active(watched_services(), r.out);
}

json SystemInfo::versions() {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!versions_.is_null() && std::chrono::steady_clock::now() - versions_at_ < std::chrono::seconds(30))
      return versions_;
  }
  struct Tool {
    const char* key;
    std::vector<std::string> argv;
  };
  const std::vector<Tool> tools = {
      {"bluetoothd", {"bluetoothd", "-v"}},     {"pipewire", {"pipewire", "--version"}},
      {"wireplumber", {"wireplumber", "--version"}}, {"bluealsa", {"bluealsa", "-V"}},
      {"btmon", {"btmon", "--version"}},
  };
  json v = json::object();
  for (const Tool& t : tools) {
    // bluetoothd lives in libexec, not on PATH.
    std::vector<std::string> argv = t.argv;
    if (argv[0] == "bluetoothd") {
      const std::string p = find_exec("bluetoothd", "/usr/libexec/bluetooth:/usr/lib/bluetooth");
      if (!p.empty()) argv[0] = p;
    }
    const ExecResult r = run_cmd(argv, 3000, 4096);
    v[t.key] = r.started && !r.timed_out ? json(parse_version(r.out + "\n" + r.err)) : json(nullptr);
  }
  v["kernel"] = uname_string();
  v["os"] = os_release();
  std::lock_guard<std::mutex> lk(m_);
  versions_ = v;
  versions_at_ = std::chrono::steady_clock::now();
  return v;
}

json SystemInfo::full() {
  json j = health();
  j["versions"] = versions();
  j["services"] = services();
  j["bluetoothd_args"] = read_bluetoothd_args(data_dir_);
  j["bluetoothd_noplugin"] = read_bluetoothd_noplugin(data_dir_);
  j["data_dir"] = data_dir_;
  j["board"] = env_json("/etc/btbench/board.env");
  j["device"] = env_json("/etc/btbench/device.env");
  return j;
}

}  // namespace btb
