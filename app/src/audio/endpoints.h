#pragma once

// Where a stream can play to and capture from (GET /api/audio/endpoints), from whichever stack
// owns the audio: PipeWire's nodes (pw-dump), or, without PipeWire, ALSA devices — BlueALSA's PCMs
// for the Bluetooth transports BlueZ has configured, and the snd-aloop loopback card. Pure
// functions of their inputs, unit-tested; audio/engine.cpp runs the tools.

#include <nlohmann/json.hpp>
#include <string>

namespace btb::audio {

// One endpoint: {id, backend: "pipewire"|"alsa", direction: "sink"|"source", label, kind:
// "bluetooth"|"loopback"|"hardware"|"virtual", address|null, profile, codec, state, default,
// rate|null, channels|null}. `id` is what a stream names as its target: PipeWire's node.name, or
// the ALSA device string ("bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp", "hw:Loopback,0,0").

// pw-dump's output (an array of objects) → {sinks:[...], sources:[...]}, with `default` set on
// the current default sink and source (the "default" metadata). Monitor-only and video nodes, and
// streams (an application's own output) are left out.
nlohmann::json endpoints_from_pw_dump(const nlohmann::json& dump);

// GET /api/media's transports → the BlueALSA PCMs for them: a transport where the board is the
// A2DP source (UUID 110a) is a sink to play into, one where it is the sink (110b) a source to
// capture; HFP/HSP transports are both, over SCO. `loopback`: snd-aloop is loaded, so
// hw:Loopback,0,0 (play) and hw:Loopback,1,0 (capture what was played into it) are there too.
nlohmann::json endpoints_from_alsa(const nlohmann::json& media, bool loopback);

// What an endpoint id may contain before it becomes a command-line argument: node names and ALSA
// device strings use [A-Za-z0-9_.:,=-] and nothing else (no spaces, quotes or option dashes at the
// start), and no ALSA device here needs more.
bool endpoint_id_ok(const std::string& id);

}  // namespace btb::audio
