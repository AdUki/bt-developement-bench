#pragma once

// Objects the daemon exports for BlueZ to call (advertisements, the local GATT application) have
// properties that depend on what the operator asked for: an advertisement without a local name
// must not have a LocalName property at all, since BlueZ would advertise an empty one. sd-bus
// vtables are normally static arrays; this builds one at runtime with only the members wanted.
//
// The vtable and every string it points to must outlive the slot it is registered with, so a
// builder is kept alive next to its sd_bus_slot.

#include <systemd/sd-bus.h>

#include <deque>
#include <string>
#include <vector>

namespace btb {

class VtableBuilder {
 public:
  VtableBuilder() { v_.push_back(SD_BUS_VTABLE_START(0)); }
  VtableBuilder(const VtableBuilder&) = delete;
  VtableBuilder& operator=(const VtableBuilder&) = delete;

  void prop(const std::string& name, const char* sig, sd_bus_property_get_t get, bool emits_change) {
    v_.push_back(SD_BUS_PROPERTY(keep(name), sig, get, 0,
                                 emits_change ? SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE
                                              : SD_BUS_VTABLE_PROPERTY_CONST));
  }
  void method(const char* name, const char* sig, const char* result, sd_bus_message_handler_t h) {
    v_.push_back(SD_BUS_METHOD(name, sig, result, h, SD_BUS_VTABLE_UNPRIVILEGED));
  }
  const sd_bus_vtable* finish() {
    v_.push_back(SD_BUS_VTABLE_END);
    return v_.data();
  }

 private:
  const char* keep(const std::string& s) {
    strings_.push_back(s);
    return strings_.back().c_str();
  }

  std::vector<sd_bus_vtable> v_;
  std::deque<std::string> strings_;  // a deque: growing it never moves what was kept
};

}  // namespace btb
