#pragma once

// The kernel's Bluetooth management interface, read-only: what the adapters can do and which
// experimental features the kernel offers, asked over a mgmt socket (HCI_CHANNEL_CONTROL) the way
// `btmgmt info` and `btmgmt expinfo` ask. These are commands the kernel answers on an untrusted
// socket too, so no capability is needed.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb {

// mgmt's 128-bit UUIDs travel little-endian: the bytes reversed, written the usual way.
std::string mgmt_uuid_str(const uint8_t* le16);
// The names of the bits set in a Read Controller Information settings word ("br/edr", "le",
// "cis-central", ...), btmgmt's names; an unknown bit as "bit26".
std::vector<std::string> mgmt_settings_names(uint32_t bits);

// {"controllers": [{"index": 0, "address": "44:A3:BB:36:5E:2E", "manufacturer": 2, "version": 13,
//                   "supported": ["powered", ...], "current": [...],
//                   "experimental": [{"uuid": "...", "flags": 1}]}],
//  "experimental": [{"uuid": "...", "flags": 1}]}   ← the global ones (no controller)
// flags: bit 0 the feature is on, bit 1 setting it changes the supported settings. False with a
// reason when there is no Bluetooth in the kernel (or no mgmt socket).
bool mgmt_read_controllers(nlohmann::json* out, std::string* err, int timeout_ms = 1000);

}  // namespace btb
