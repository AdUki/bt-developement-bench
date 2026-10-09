#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace btb {

// The supply. Undervoltage on a Zero W corrupts the Bluetooth UART long before anything else
// notices, so a bench reading taken under it is suspect. Mainline has no vcgencmd/get_throttled:
// the raspberrypi-hwmon driver ("rpi_volt") raises in0_lcrit_alarm instead.
struct PowerInfo {
  bool available = false;     // no rpi_volt hwmon: a PC, or a kernel without the driver
  bool under_voltage = false;  // right now
  bool seen = false;           // at some point since the daemon started watching
};

struct MemInfo {
  uint64_t total_kb = 0;
  uint64_t available_kb = 0;  // what a new allocation can actually get (not just MemFree)
  uint64_t used_kb = 0;       // total - available
};

struct SysInfo {
  float cpu_pct = 0.0f;  // whole machine
  MemInfo mem;
  PowerInfo power;
  float temp_c = -1.0f;  // negative when the platform has no thermal zone
  double uptime_s = 0.0;
  double load1 = 0.0;
  std::string hostname;
  std::vector<std::string> ips;  // "usb0 10.55.0.1"
};

// CPU load is a delta between two readings of /proc/stat, so whoever samples it owns the previous
// reading — exactly one owner, sampling on a steady cadence (the 1 Hz system publisher). Two callers
// would consume each other's window and both read garbage. Not thread-safe by design.
class CpuSampler {
 public:
  float sample();

 private:
  uint64_t prev_idle_ = 0;
  uint64_t prev_total_ = 0;
  bool primed_ = false;
};

// Everything except CPU load: stateless (but for the latched undervoltage flag), any thread.
SysInfo sample_sysinfo();

// The kernel and the distribution, for the System page: `uname -srvm` and os-release's PRETTY_NAME.
std::string uname_string();
std::string os_release();

// This machine's IPv4 addresses, without the interface names: the hosts the console is served as.
std::vector<std::string> local_ipv4();

}  // namespace btb
