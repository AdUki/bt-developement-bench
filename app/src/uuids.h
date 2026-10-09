#pragma once

#include <map>
#include <mutex>
#include <string>

namespace btb {

// "180d", "0x180D", "0000180d", "0000180d-0000-1000-8000-00805f9b34fb" → the full 128-bit form in
// lowercase. "" when it is none of these. What every UUID the API takes goes through, so a 16-bit
// one typed by hand and the 128-bit one BlueZ reports compare equal.
std::string uuid_full(const std::string& in);
// The other way: "180d" for a UUID in the Bluetooth Base UUID with a 16-bit value, 8 hex digits for
// a 32-bit one, else the full lowercase form.
std::string uuid_short(const std::string& in);

// The names of www/uuids.json — the same file the console reads — so the device UUID list and the
// GATT tree come with names in the API too. Loaded once at startup; lookups are thread-safe.
class UuidNames {
 public:
  bool load(const std::string& path, std::string* err);
  bool load_text(const std::string& text, std::string* err);
  // "" when unknown.
  std::string name(const std::string& uuid) const;
  size_t size() const;

 private:
  mutable std::mutex m_;
  std::map<std::string, std::string> names_;  // by uuid_full()
};

UuidNames& uuid_names();

}  // namespace btb
