#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb {

// What a MediaEndpoint1's Capabilities or a MediaTransport1's Configuration says, in words.
// BlueZ hands both over as the raw codec information element; this is the part of a2dp-codecs.h
// and the BAP LTVs a developer would otherwise decode by hand from btmon.
//
// `codec` is MediaEndpoint1/MediaTransport1.Codec: the A2DP codec type (0x00 SBC, 0x02 AAC,
// 0xff vendor) or, for LE Audio, the BAP coding format (0x06 LC3). `caps` is true for an endpoint's
// capabilities (each field a set of options) and false for a transport's configuration (one value
// each, and a bitrate where that follows from it).
//
// The result always has "name" (e.g. "SBC", "aptX HD", "vendor 0x0000012d:0x00aa") and "summary"
// (one line), plus the decoded fields; an element too short for its codec gets "error" instead.
nlohmann::json decode_codec(uint8_t codec, const std::vector<uint8_t>& data, bool caps);
std::string codec_name(uint8_t codec, const std::vector<uint8_t>& data);

// The SBC bitrate in bit/s for one configuration, as the encoder computes its frame length
// (A2DP spec 12.9). 0 for a combination that is not a single configuration.
unsigned sbc_bitrate(unsigned rate, const std::string& channel_mode, unsigned blocks,
                     unsigned subbands, unsigned bitpool);

}  // namespace btb
