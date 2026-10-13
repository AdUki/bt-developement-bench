#pragma once

// Where a stream can play to and capture from (GET /api/audio/endpoints), from whichever stack
// owns the audio: PipeWire's nodes (pw-dump), or, with PipeWire stopped or not answering, the plain
// ALSA devices — just the snd-aloop loopback card, since no other ALSA device on the bench speaks
// Bluetooth. Pure functions of their inputs, unit-tested; audio/engine.cpp runs the tools.

#include <nlohmann/json.hpp>
#include <string>

namespace btb::audio {

// One endpoint: {id, backend: "pipewire"|"alsa", direction: "sink"|"source", label, kind:
// "bluetooth"|"loopback"|"hardware"|"virtual", address|null, profile, codec, state, default,
// rate|null, channels|null, adapter|null}. `id` is what a stream names as its target: PipeWire's
// node.name, or the ALSA device string ("hw:Loopback,0,0"). A Bluetooth endpoint's `adapter` is the
// hciN its device is connected on; a device connected on two adapters has two nodes of one
// node.name, and those have their object.serial as `id` and the adapter in the label.

// pw-dump's stdout → one array of objects. pw-dump prints the dump as a JSON array, and when
// objects change while it runs it prints more arrays after it (the updates, keyed by id, with
// null info for a removed object): they are merged in, later wins.
nlohmann::json parse_pw_dump(const std::string& text);

// pw-dump's output (an array of objects) → {sinks:[...], sources:[...]}, with `default` set on
// the current default sink and source (the "default" metadata). Monitor-only and video nodes, and
// streams (an application's own output) are left out.
nlohmann::json endpoints_from_pw_dump(const nlohmann::json& dump);

// The ALSA endpoints when PipeWire is not running. `loopback`: snd-aloop is loaded, so
// hw:Loopback,0,0 (play) and hw:Loopback,1,0 (capture what was played into it) are there; without
// it there is nothing, and the endpoint list says backend "none".
nlohmann::json endpoints_from_alsa(bool loopback);

// The codec picker: pw-dump's Bluetooth devices (PipeWire "cards") with their profiles, as
// {devices:[{id, name, label, address, active:{...profile}|null, profiles:[{index, name,
// description, available, kind, codec}]}]}. In PipeWire a profile *is* the codec choice
// ("a2dp-sink-sbc_xq": send A2DP with SBC-XQ), so choosing one is how a codec is picked. kind:
// "a2dp" (send to the device), "a2dp-duplex" (both ways), "gateway" (receive the device's A2DP,
// be its HFP gateway), "headset" (HFP/HSP as the head unit), "bap" (LE Audio), "off", "other".
// codec is the profile's codec as PipeWire names it in the description ("SBC-XQ"), "" for none.
nlohmann::json cards_from_pw_dump(const nlohmann::json& dump);
// "High Fidelity Playback (A2DP Sink, codec SBC-XQ)" → "SBC-XQ".
std::string profile_codec(const std::string& description);
std::string profile_kind(const std::string& name);

// What an endpoint id may contain before it becomes a command-line argument: node names and ALSA
// device strings use [A-Za-z0-9_.:,=-] and nothing else (no spaces, quotes or option dashes at the
// start), and no ALSA device here needs more.
bool endpoint_id_ok(const std::string& id);

}  // namespace btb::audio
