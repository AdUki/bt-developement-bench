#pragma once

// A2DP Media Codec capability decoding (AVDTP service category 7), modelled on bluez
// monitor/a2dp.c. The same bytes are a *capability* in GET_CAPABILITIES (several bits set per
// field) and a *configuration* in SET_CONFIGURATION (one bit each); the formatter lists every set
// bit, so one function serves both.

#include <cstddef>
#include <cstdint>
#include <string>

namespace btb::hci {

struct CodecInfo {
  std::string name;    // "SBC", "AAC", "aptX HD", "LDAC", "vendor 0x0000004f:0x0001", ...
  std::string config;  // "48000 Hz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2..53"
  uint32_t rate = 0;   // sample rate when exactly one is selected (RTP clock), else 0
  bool rtp = true;     // aptX, aptX LL and FastStream send raw frames, no RTP header
  bool sbc = false;    // the media payload starts with SBC's frame-count byte
};

// p points at the Media Codec capability value: media type byte, codec type byte, codec info.
bool decode_media_codec(const uint8_t* p, size_t len, CodecInfo* out);

const char* avdtp_signal_name(uint8_t id);

}  // namespace btb::hci
