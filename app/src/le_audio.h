#pragma once

// LE Audio broadcasts (Auracast) over BlueZ's BAP plugin, both ways:
//
// - the board's own: a BIG it transmits. Each adapter with a broadcast PAC (/org/bluez/hci0/
//   pac_bcast0, there once PipeWire registered its broadcast endpoints) takes SetConfiguration with
//   a BIS's codec configuration and QoS, and BlueZ hands the stream to PipeWire as a transport
//   (pac_bcast0/fd1) and PipeWire makes it a sink. The BIG goes on air when something plays into
//   that sink, and off when it is cleared (ClearConfiguration). PipeWire's own way is a fixed list
//   in WirePlumber's config (bluez5.bcast_source.config), read once at its start; this way a
//   broadcast is started and stopped like a stream, with no restart.
// - the ones in range: while the adapter scans, BlueZ syncs to a broadcaster's periodic advertising
//   for a moment, reads its BASE and makes one endpoint and transport per BIS
//   (dev_.../sid0/bis1/fd0). Select() on a transport is "receive this one": BlueZ syncs to the BIG
//   when PipeWire acquires it, and PipeWire plays it like any Bluetooth source.
//
// Pure functions over the object snapshot, unit-tested; api_le_audio.cpp makes the calls.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb {

// A BAP broadcast audio stream configuration (BAP 1.0.1, table 6.4): "48_2_1" is 48 kHz, 10 ms
// frames, low latency; "48_2_2" the same with the high-reliability QoS.
struct LeBcastPreset {
  std::string name;
  unsigned rate = 0;
  unsigned frame_us = 0;
  uint16_t octets = 0;  // per channel per frame
  uint8_t rtn = 0;
  uint16_t latency_ms = 0;
};
// Every preset this takes (8, 16, 24, 32 and 48 kHz, both QoS sets); 44.1 kHz is left out: its
// frames are framed with a fractional interval.
const std::vector<LeBcastPreset>& le_bcast_presets();

// What POST /api/le-audio/broadcasts asks for, checked: the codec configuration, metadata and QoS
// of each BIS of one BIG.
struct LeBcastConfig {
  LeBcastPreset preset;
  unsigned channels = 2;   // 1 (front left, or right) or 2 (front left + right) in the BIS
  bool right = false;      // a one-channel BIS that carries the right channel
  uint8_t big = 0;
  bool encrypted = false;
  std::vector<uint8_t> code = std::vector<uint8_t>(16, 0);  // the Broadcast Code, zeros for none
  uint32_t presentation_delay_us = 40000;
  std::vector<uint8_t> capabilities;  // LC3 LTVs: rate, duration, allocation, octets
  std::vector<uint8_t> metadata;      // streaming context: media
  uint16_t sdu = 0;
  uint32_t interval_us = 0;
};
// {preset ("48_2_1"), channels (1|2), location ("left" | "right", for one channel), code ("" none; text of up to 16 bytes, or 0x and 32 hex
// digits), presentation_delay_us}. `big` is the BIG handle to use (the caller picks a free one).
bool le_bcast_config_from_json(const nlohmann::json& j, uint8_t big, LeBcastConfig* out, std::string* err);
// A Broadcast Code: the text as UTF-8 padded with zeros (as Auracast apps take it), or 0x + 32 hex
// digits as the 16 bytes in the order they go on air.
bool le_bcast_code(const std::string& text, std::vector<uint8_t>* out, std::string* err);

// LE Audio metadata LTVs (Assigned Numbers 6.12.6) in the API's words, only those present:
// {contexts (Streaming_Audio_Contexts bits), program_info (what plays, UTF-8), language (ISO 639-3)}.
nlohmann::json le_metadata_json(const std::vector<uint8_t>& m);

// GET /api/le-audio from the object snapshot, mgmt's controllers (mgmt_read_controllers: which
// adapter can broadcast and receive) and the audio endpoints (GET /api/audio/endpoints: the node
// each transport plays through):
// {adapters: [{adapter, address, name, broadcaster, receiver, pac, discovering}],
//  broadcasts: [{path, adapter, big, bis, state, codec, qos, encrypted, node}],
//  sources: [{address, device, adapter, name, rssi, broadcast_id, encrypted, paired, connected,
//             streams: [{path, endpoint, big, bis, state, codec, location, metadata, program_info,
//                        encrypted, node}]}]}
// A stream's program_info is its metadata's Program_Info, null without one.
nlohmann::json model_le_audio(const nlohmann::json& objs, const nlohmann::json& controllers,
                              const nlohmann::json& endpoints);

// The lowest BIG handle none of `broadcasts` (model_le_audio's) uses; 0xff when all are taken.
uint8_t le_free_big(const nlohmann::json& broadcasts);

}  // namespace btb
