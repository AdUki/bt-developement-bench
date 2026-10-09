#include <time.h>

#include <cstdio>

#include "logd.h"

namespace btb::logd {

namespace {

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string basename_of(const std::string& p) {
  const auto at = p.find_last_of('/');
  return at == std::string::npos ? p : p.substr(at + 1);
}

struct tm local(uint64_t us) {
  const time_t t = static_cast<time_t>(us / 1000000);
  struct tm tm {};
  localtime_r(&t, &tm);
  return tm;
}

// "hh:mm:ss.uuuuuu" (PipeWire, WirePlumber).
std::string clock_us(uint64_t us) {
  const struct tm tm = local(us);
  char b[32];
  std::snprintf(b, sizeof b, "%02d:%02d:%02d.%06u", tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<unsigned>(us % 1000000));
  return b;
}

// "YYYYMMDD hh:mm:ss.mmm" (the nsdk and bluez groups).
std::string stamp_ms(uint64_t us) {
  const struct tm tm = local(us);
  char b[40];
  std::snprintf(b, sizeof b, "%04d%02d%02d %02d:%02d:%02d.%03u", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<unsigned>(us / 1000 % 1000));
  return b;
}

// A message's own "topic: " prefix (PipeWire puts "spa.alsa: " in front, WirePlumber
// "wp-device: "): a word without spaces, then ": ".
bool split_topic(const std::string& m, std::string* topic, std::string* rest) {
  const auto at = m.find(": ");
  if (at == std::string::npos || at == 0 || at > 40 || m.find(' ') < at) return false;
  *topic = m.substr(0, at);
  *rest = m.substr(at + 2);
  return true;
}

std::string pad_right(std::string s, size_t n) {
  if (s.size() < n) s.append(n - s.size(), ' ');
  return s;
}

std::string pad_left(const std::string& s, size_t n) { return s.size() < n ? std::string(n - s.size(), ' ') + s : s; }

}  // namespace

Route route(const Entry& e) {
  if (e.transport == "kernel") return {"kernel.log", Format::nsdk, false};
  if (e.unit == "bluetooth.service" || e.ident == "bluetoothd") return {"bluez.log", Format::bluez, false};
  if (e.unit == "pipewire.service" || e.ident == "pipewire") return {"pipewire.log", Format::pipewire, false};
  if (e.unit == "wireplumber.service" || e.ident == "wireplumber") return {"wireplumber.log", Format::wireplumber, false};
  if (e.ident == "wpa_supplicant" || starts(e.unit, "wpa_supplicant")) return {"wpasupplicant.log", Format::wpasupplicant, false};
  if (e.unit == "btbenchd.service" || e.ident == "btbenchd") return {"btbenchd.log", Format::nsdk, false};
  return {"system.log", Format::nsdk, true};
}

std::string format(const Entry& e, const Route& r) {
  const int p = e.priority;
  switch (r.format) {
    case Format::bluez: {
      // bluetoothd[13077]: 20220309 12:06:48.280 DEBUG src/adapter.c:adapter_service_insert() /org/bluez/hci0
      const char* lvl = p >= 7 ? "DEBUG" : p >= 5 ? "INFO" : p == 4 ? "WARN" : "ERROR";
      return "bluetoothd[" + std::to_string(e.pid) + "]: " + stamp_ms(e.realtime_us) + " " + lvl + " " + e.message;
    }
    case Format::pipewire: {
      // [I][10:44:56.111404] pw.conf      | [          conf.c:  423 conf_load()] loaded config ...
      const char lvl = p >= 7 ? 'D' : p >= 5 ? 'I' : p == 4 ? 'W' : 'E';
      std::string topic, rest = e.message;
      if (!split_topic(e.message, &topic, &rest)) topic = e.topic.empty() ? e.ident : e.topic;
      std::string where;
      if (!e.code_file.empty())
        where = "[" + pad_left(basename_of(e.code_file), 16) + ":" + pad_left(std::to_string(e.code_line), 5) + " " +
                e.code_func + "()] ";
      return std::string("[") + lvl + "][" + clock_us(e.realtime_us) + "] " + pad_right(topic, 12) + " | " + where + rest;
    }
    case Format::wireplumber: {
      // I 16:45:19.152172    m-lua-scripting config.c:104:load_file: loading config file ...
      const char lvl = p >= 7 ? 'D' : p == 6 ? 'I' : p == 5 ? 'M' : p == 4 ? 'W' : p == 3 ? 'E' : 'C';
      std::string topic = e.topic, rest = e.message, prefix;
      if (split_topic(e.message, &prefix, &rest) && (topic.empty() || prefix == topic)) topic = prefix;
      else rest = e.message;
      if (topic.empty()) topic = e.ident;
      const std::string where = basename_of(e.code_file.empty() ? "?" : e.code_file) + ":" + std::to_string(e.code_line) +
                                ":" + (e.code_func.empty() ? "?" : e.code_func) + ":";
      return std::string(1, lvl) + " " + clock_us(e.realtime_us) + " " + pad_left(topic, 18) + " " + where + " " + rest;
    }
    case Format::wpasupplicant: {
      // 1649400051.070940: dbus: Unregister BSS object ...
      char b[32];
      std::snprintf(b, sizeof b, "%llu.%06u: ", static_cast<unsigned long long>(e.realtime_us / 1000000),
                    static_cast<unsigned>(e.realtime_us % 1000000));
      return b + e.message;
    }
    case Format::nsdk:
    default: {
      // 20220403 11:35:59.169 [778.781] INFO Networking: Enter Networking::requestWifiResource ...
      const char* lvl = p >= 7 ? "DEBUG" : p >= 5 ? "INFO" : p == 4 ? "WARN" : p == 3 ? "ERROR" : "FATAL";
      const long tid = e.tid ? e.tid : e.pid;
      return stamp_ms(e.realtime_us) + " [" + std::to_string(e.pid) + "." + std::to_string(tid) + "] " + lvl + " " +
             (r.prefix_ident ? e.ident + ": " : "") + e.message;
    }
  }
}

}  // namespace btb::logd
