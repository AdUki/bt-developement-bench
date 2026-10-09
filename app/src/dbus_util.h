#pragma once

#include <systemd/sd-bus.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace btb {

// Reads the next complete value of `m` into JSON: numbers, strings and booleans as themselves,
// byte arrays as arrays of numbers, dictionaries as objects (keyed by their key's text: a{qv}'s
// 0x004c becomes "76"), structs as arrays and variants as what they hold. One reader for every
// reply, so BlueZ's a{oa{sa{sv}}} is a few lookups rather than a nest of container calls.
// Returns 0 at the end of the enclosing container, 1 after a value, a negative errno on a
// malformed message.
int read_json(sd_bus_message* m, nlohmann::json* out);

// One value of an a{sv} the daemon builds (discovery filters, GATT read/write options). Only the
// types those need.
struct DVar {
  char type = 's';  // s, b, q, n, u, i, y; 'A' = as, 'Y' = ay
  std::string s;
  int64_t i = 0;
  std::vector<std::string> as;
  std::vector<uint8_t> ay;

  static DVar str(std::string v) { DVar d; d.type = 's'; d.s = std::move(v); return d; }
  static DVar boolean(bool v) { DVar d; d.type = 'b'; d.i = v; return d; }
  static DVar u16(uint16_t v) { DVar d; d.type = 'q'; d.i = v; return d; }
  static DVar i16(int16_t v) { DVar d; d.type = 'n'; d.i = v; return d; }
  static DVar u32(uint32_t v) { DVar d; d.type = 'u'; d.i = v; return d; }
  static DVar strv(std::vector<std::string> v) { DVar d; d.type = 'A'; d.as = std::move(v); return d; }
  static DVar bytes(std::vector<uint8_t> v) { DVar d; d.type = 'Y'; d.ay = std::move(v); return d; }
};
using DDict = std::vector<std::pair<std::string, DVar>>;

int append_variant(sd_bus_message* m, const DVar& v);
int append_dict(sd_bus_message* m, const DDict& d);
int append_bytes(sd_bus_message* m, const std::vector<uint8_t>& b);

// A BlueZ error as an operator reads it: BlueZ's own message, with the error name's meaning where
// the message alone says little ("Page Timeout" stays, "org.bluez.Error.NotReady" becomes "the
// adapter is not powered").
std::string friendly(const sd_bus_error* e);
bool is_error(const sd_bus_error* e, const char* name);

// "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service0010/char0011" → "AA:BB:CC:DD:EE:FF".
std::string address_from_path(const std::string& path);
// "/org/bluez/hci0/..." → "hci0".
std::string adapter_from_path(const std::string& path);

// JSON helpers for BlueZ's property dicts.
std::string str_of(const nlohmann::json& o, const char* key);
bool bool_of(const nlohmann::json& o, const char* key);
long long num_of(const nlohmann::json& o, const char* key, long long fallback = 0);
std::vector<uint8_t> bytes_of(const nlohmann::json& o, const char* key);
std::vector<std::string> strv_of(const nlohmann::json& o, const char* key);

}  // namespace btb
