#pragma once

// A2DP Media Codec capability decoding (AVDTP service category 7), modelled on bluez
// monitor/a2dp.c. The same bytes are a *capability* in GET_CAPABILITIES (several bits set per
// field) and a *configuration* in SET_CONFIGURATION (one bit each); the formatter lists every set
// bit, so one function serves both.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace btb::hci {

struct CodecInfo {
  std::string name;    // "SBC", "AAC", "aptX HD", "LDAC", "vendor 0x0000004f:0x0001", ...
  std::string config;  // "48000 Hz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53"
  uint32_t rate = 0;   // sample rate when exactly one is selected (RTP clock), else 0
  bool rtp = true;     // aptX, aptX LL and FastStream send raw frames, no RTP header
  bool sbc = false;    // the media payload starts with SBC's frame-count byte
  // The same, one field at a time, for the packet view's field tree: {"Sampling frequency",
  // "44100 Hz"}, {"Vendor", "0x000005f1 (The Linux Foundation)"}, ...
  std::vector<std::pair<std::string, std::string>> fields;
};

// p points at the Media Codec capability value: media type byte, codec type byte, codec info.
bool decode_media_codec(const uint8_t* p, size_t len, CodecInfo* out);

const char* avdtp_signal_name(uint8_t id);
// A Bluetooth SIG company ID by name, for the ones the bench meets; nullptr otherwise.
const char* company_name(uint16_t id);
// AVDTP and A2DP error codes (AVDTP 8.20.6.2, A2DP 5.1.3), nullptr if unknown.
const char* avdtp_error_name(uint8_t e);

}  // namespace btb::hci
