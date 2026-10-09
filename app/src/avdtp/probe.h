#pragma once

// AVDTP browsing (docs/api.md "AVDTP"): a device's stream endpoints as the device itself states
// them, read over an AVDTP signalling channel of btbenchd's own — DISCOVER, then
// GET_ALL_CAPABILITIES (GET_CAPABILITIES for an AVDTP 1.0 device) of every SEP — and decoded:
// media type and role, in-use, and each service capability (media transport, reporting, recovery,
// content protection, header compression, multiplexing, the media codec with a2dp_codec's decoder,
// delay reporting).
//
// This needs nothing from the audio stack and sees what BlueZ's MediaEndpoint1 objects do not:
// SEPs that are in use, the capabilities besides the codec, and a device that is not connected
// over A2DP at all. Most devices accept one signalling channel at a time, so while A2DP is up
// (BlueZ owns the channel) a probe is usually refused; the answer says so and the console shows
// BlueZ's endpoints instead.
//
// The message builders and parsers are pure (unit-tested); probe() does the socket part.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb::avdtp {

constexpr uint8_t kDiscover = 0x01;
constexpr uint8_t kGetCapabilities = 0x02;
constexpr uint8_t kGetAllCapabilities = 0x0c;

// A single-packet signalling command: transaction label, signal id, optional ACP SEID.
std::vector<uint8_t> command(uint8_t label, uint8_t signal, int acp_seid = -1);

struct Reply {
  bool ok = false;        // a response accept for (label, signal)
  bool rejected = false;  // a response reject (error_code set) or general reject
  uint8_t error_code = 0;
  std::vector<uint8_t> payload;  // after the 2-byte header
};
// Checks the header against the command it answers.
Reply parse_reply(const std::vector<uint8_t>& pkt, uint8_t label, uint8_t signal);

// DISCOVER's accept → [{seid, in_use, media_type ("audio"|"video"|"multimedia"|n), role
// ("source"|"sink")}].
nlohmann::json parse_discover(const std::vector<uint8_t>& payload);
// GET_(ALL_)CAPABILITIES' accept → [{category, name, hex, ...decoded}] and, for the media codec,
// the codec decoded (same shape as GET /api/media's endpoint codec).
nlohmann::json parse_capabilities(const std::vector<uint8_t>& payload);
const char* error_name(uint8_t code);

// Connects to `address` on PSM 25 (paging the device if needed), asks, and closes.
// {address, seps:[{...discover, capabilities:[...], codec:{...}|null, error?}], took_ms}.
bool probe(const std::string& address, int timeout_ms, nlohmann::json* out, std::string* err);

}  // namespace btb::avdtp
