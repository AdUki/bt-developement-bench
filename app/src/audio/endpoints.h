#pragma once

// Where a stream can play to and capture from (GET /api/audio/endpoints), from whichever stack
// owns the audio: PipeWire's nodes (pw-dump), or, without PipeWire (audio mode "none"), the plain
// ALSA devices — just the snd-aloop loopback card, since no other ALSA device on the bench speaks
// Bluetooth. Pure functions of their inputs, unit-tested; audio/engine.cpp runs the tools.

#include <nlohmann/json.hpp>
#include <string>

namespace btb::audio {

// One endpoint: {id, backend: "pipewire"|"alsa", direction: "sink"|"source", label, kind:
// "bluetooth"|"loopback"|"hardware"|"virtual", address|null, profile, codec, state, default,
// rate|null, channels|null}. `id` is what a stream names as its target: PipeWire's node.name, or
// the ALSA device string ("hw:Loopback,0,0").

// pw-dump's output (an array of objects) → {sinks:[...], sources:[...]}, with `default` set on
// the current default sink and source (the "default" metadata). Monitor-only and video nodes, and
// streams (an application's own output) are left out.
nlohmann::json endpoints_from_pw_dump(const nlohmann::json& dump);

// The ALSA endpoints when PipeWire is not running. `loopback`: snd-aloop is loaded, so
// hw:Loopback,0,0 (play) and hw:Loopback,1,0 (capture what was played into it) are there; without
// it there is nothing, and the endpoint list says backend "none".
nlohmann::json endpoints_from_alsa(bool loopback);

// What an endpoint id may contain before it becomes a command-line argument: node names and ALSA
// device strings use [A-Za-z0-9_.:,=-] and nothing else (no spaces, quotes or option dashes at the
// start), and no ALSA device here needs more.
bool endpoint_id_ok(const std::string& id);

}  // namespace btb::audio
