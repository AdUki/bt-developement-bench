#include "util/sysinfo.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <sstream>

#include "util/strings.h"

namespace btb {

namespace {

std::string read_line(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  if (f) std::getline(f, s);
  return trim(s);
}

// The SoC's zone: thermal_zone0 on a Pi. A PC has several and the first is as good as any for a
// bench that only wants to see the board is not cooking.
float sample_temp() {
  const std::string t = read_line("/sys/class/thermal/thermal_zone0/temp");
  if (t.empty()) return -1.0f;
  return static_cast<float>(std::strtol(t.c_str(), nullptr, 10)) / 1000.0f;
}

double sample_uptime() {
  std::ifstream f("/proc/uptime");
  double up = 0.0;
  if (f) f >> up;
  return up;
}

double sample_load() {
  std::ifstream f("/proc/loadavg");
  double l = 0.0;
  if (f) f >> l;
  return l;
}

MemInfo sample_mem() {
  MemInfo m;
  std::ifstream f("/proc/meminfo");
  if (!f) return m;
  std::string key, unit;
  uint64_t value = 0;
  while (f >> key >> value >> unit) {
    if (key == "MemTotal:") m.total_kb = value;
    // MemAvailable, not MemFree: the kernel's estimate of what an allocation can get, counting
    // reclaimable page cache. MemFree on a box that has run a while means nothing.
    else if (key == "MemAvailable:") m.available_kb = value;
    if (m.total_kb && m.available_kb) break;
  }
  if (m.total_kb >= m.available_kb) m.used_kb = m.total_kb - m.available_kb;
  return m;
}

// Found once: hwmon numbering is stable for a boot, and the scan is a directory walk.
std::string find_rpi_volt() {
  DIR* d = opendir("/sys/class/hwmon");
  if (!d) return {};
  std::string found;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string dir = std::string("/sys/class/hwmon/") + e->d_name;
    if (read_line(dir + "/name") == "rpi_volt") {
      found = dir + "/in0_lcrit_alarm";
      break;
    }
  }
  closedir(d);
  return found;
}

std::atomic<bool> g_uv_seen{false};

PowerInfo sample_power() {
  static const std::string path = find_rpi_volt();
  PowerInfo p;
  if (path.empty()) return p;
  const std::string v = read_line(path);
  if (v.empty()) return p;
  p.available = true;
  p.under_voltage = v != "0";
  // The alarm is level-triggered and a brown-out is often a few milliseconds long; the daemon
  // latches it so a dip between two 1 Hz samples is still reported... when the sample catches it.
  if (p.under_voltage) g_uv_seen = true;
  p.seen = g_uv_seen.load();
  return p;
}

}  // namespace

float CpuSampler::sample() {
  std::ifstream f("/proc/stat");
  std::string tag;
  uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  if (!(f >> tag >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal) || tag != "cpu")
    return 0.0f;
  const uint64_t i = idle + iowait;
  const uint64_t t = user + nice + sys + i + irq + softirq + steal;
  float pct = 0.0f;
  // The first call has no previous reading: report 0 rather than the average since boot.
  if (primed_ && t > prev_total_) {
    const uint64_t dt = t - prev_total_;
    const uint64_t di = i - prev_idle_;
    pct = 100.0f * static_cast<float>(dt - di) / static_cast<float>(dt);
  }
  prev_idle_ = i;
  prev_total_ = t;
  primed_ = true;
  return pct;
}

std::vector<std::string> local_ipv4() {
  std::vector<std::string> out;
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return out;
  for (ifaddrs* i = ifa; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    char ip[INET_ADDRSTRLEN] = {0};
    auto* addr = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
    if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) out.push_back(ip);
  }
  freeifaddrs(ifa);
  return out;
}

SysInfo sample_sysinfo() {
  SysInfo s;
  s.temp_c = sample_temp();
  s.uptime_s = sample_uptime();
  s.load1 = sample_load();
  s.mem = sample_mem();
  s.power = sample_power();

  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) == 0) s.hostname = host;

  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0) {
    for (ifaddrs* i = ifa; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
      if (i->ifa_flags & IFF_LOOPBACK) continue;
      char ip[INET_ADDRSTRLEN] = {0};
      auto* addr = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
      if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)))
        s.ips.push_back(std::string(i->ifa_name) + " " + ip);
    }
    freeifaddrs(ifa);
  }
  return s;
}

std::string uname_string() {
  utsname u{};
  if (uname(&u) != 0) return {};
  return std::string(u.sysname) + " " + u.release + " " + u.version + " " + u.machine;
}

std::string os_release() {
  std::ifstream f("/etc/os-release");
  std::stringstream ss;
  ss << f.rdbuf();
  const auto env = parse_env(ss.str());
  const auto it = env.find("PRETTY_NAME");
  return it != env.end() ? it->second : std::string{};
}

}  // namespace btb
