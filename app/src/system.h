#pragma once

#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "util/sysinfo.h"

namespace btb {

// The services the System page shows, in the order it shows them.
const std::vector<std::string>& watched_services();
// Units the API may restart: the stack under test, and the bench's own services.
bool restartable_service(const std::string& unit);

// `systemctl is-active a b c` prints one state per line, in order; a unit it does not know is
// "inactive" (or nothing at all on a box without systemd). {unit: state}.
nlohmann::json parse_is_active(const std::vector<std::string>& units, const std::string& out);
// "bluetoothd 5.84" / "5.84" / "pipewire\nCompiled with libpipewire 1.4.2\n..." → the version.
std::string parse_version(const std::string& out);

// BLUETOOTHD_ARGS="..." in /data/btbench/bluetoothd.env (docs/contracts.md), read by the
// bluetooth.service drop-in. Checked on the way in: one line, no quotes games, only options.
bool bluetoothd_args_ok(const std::string& args, std::string* err);
std::string read_bluetoothd_args(const std::string& data_dir);
// Replaces BLUETOOTHD_ARGS only; every other line (BLUETOOTHD_NOPLUGIN) is kept.
bool write_bluetoothd_args(const std::string& data_dir, const std::string& args, std::string* err);
std::string read_bluetoothd_noplugin(const std::string& data_dir);

// What the System page and `bench sys` show.
class SystemInfo {
 public:
  explicit SystemInfo(std::string data_dir) : data_dir_(std::move(data_dir)) {}

  // Host health: written by the 1 Hz sampler (the CPU delta has one owner), read by anyone.
  void store_sample(const SysInfo& s);
  SysInfo last_sample() const;

  // Everything: {hostname, ips, uname, os, uptime_s, load1, cpu_pct, temp_c, mem, power,
  // versions, services, bluetoothd_args, board, device}. versions are cached for a while: each is
  // a process start, and on a Zero W `pipewire --version` alone takes a noticeable fraction of a
  // second.
  nlohmann::json full();
  nlohmann::json health() const;
  nlohmann::json services() const;
  nlohmann::json versions();
  const std::string& data_dir() const { return data_dir_; }

 private:
  std::string data_dir_;
  mutable std::mutex m_;
  SysInfo sample_;
  nlohmann::json versions_;
  std::chrono::steady_clock::time_point versions_at_{};
};

nlohmann::json sysinfo_json(const SysInfo& s);

}  // namespace btb
