// The audio streams' decisions: the generators' levels and continuity, the meter, the WAV
// framing, what a stream body may say and the command lines it becomes, the endpoint lists made
// from pw-dump and from BlueZ's transports, and the UPnP parsers. The pumping itself (processes,
// pipes) is exercised with `make run` against the desktop's PipeWire and a radio stream.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/endpoints.h"
#include "audio/engine.h"
#include "audio/pcm.h"
#include "audio/upnp.h"
#include "check.h"

using json = nlohmann::json;
using namespace btb::audio;

namespace {

double rms_db(const std::vector<int16_t>& s, int ch, int c) {
  double sum = 0;
  size_t n = 0;
  for (size_t i = c; i < s.size(); i += ch, ++n) sum += static_cast<double>(s[i]) * s[i];
  return 20 * std::log10(std::sqrt(sum / n) / 32768.0);
}

void test_tone() {
  ToneGen g(48000, 2, 1000, 0, -6);
  std::vector<int16_t> a(48000 * 2);
  g.fill(a.data(), 48000);
  // A sine's RMS is 3 dB under its peak.
  CHECK_NEAR(rms_db(a, 2, 0), -9.0, 0.1);
  CHECK_NEAR(rms_db(a, 2, 1), -9.0, 0.1);
  // 1 kHz at 48 kHz: 48 samples a period, so sample 48 is sample 0 again (phase 0 → 0).
  CHECK(std::abs(a[0]) <= 1);
  CHECK(std::abs(a[96] - a[0]) <= 2);
  // Continuous across chunks: two halves equal one whole.
  ToneGen h(48000, 1, 997, 0, -1), k(48000, 1, 997, 0, -1);
  std::vector<int16_t> one(1000), two(1000);
  h.fill(one.data(), 1000);
  k.fill(two.data(), 500);
  k.fill(two.data() + 500, 500);
  int maxdiff = 0;
  for (int i = 0; i < 1000; ++i) maxdiff = std::max(maxdiff, std::abs(one[i] - two[i]));
  CHECK(maxdiff <= 1);
  // The right channel at its own frequency: a different signal.
  ToneGen lr(48000, 2, 1000, 500, -6);
  std::vector<int16_t> b(200);
  lr.fill(b.data(), 100);
  CHECK(b[2 * 30] != b[2 * 30 + 1]);
}

void test_sweep_noise() {
  SweepGen s(8000, 1, 100, 1000, 1.0, true, false, -6);
  std::vector<int16_t> v(9000);
  s.fill(v.data(), 9000);
  CHECK(s.done());
  CHECK_EQ(v[8500], int16_t{0});  // silence once a non-repeating sweep is done
  for (const char* c : {"white", "pink", "brown"}) {
    NoiseGen::Color col;
    CHECK(noise_color(c, &col));
    NoiseGen n(1, col, -20);
    std::vector<int16_t> x(96000);
    n.fill(x.data(), 96000);
    // The level is the RMS, whatever the colour.
    const double r = rms_db(x, 1, 0);
    if (std::abs(r + 20) > 1.0) std::printf("  %s noise RMS %.2f dB\n", c, r);
    CHECK_NEAR(r, -20.0, 1.0);
  }
  NoiseGen::Color col;
  CHECK(!noise_color("blue", &col));
}

void test_gain_meter() {
  std::vector<int16_t> s = {1000, -1000, 30000, -30000};
  apply_gain(s.data(), s.size(), 2.0);
  CHECK_EQ(s[0], int16_t{2000});
  CHECK_EQ(s[2], int16_t{32767});   // saturates
  CHECK_EQ(s[3], int16_t{-32768});
  std::vector<int16_t> st = {100, 300, -100, -300};
  CHECK_EQ(downmix(st.data(), 2), size_t(2));
  CHECK_EQ(st[0], int16_t{200});
  CHECK_EQ(st[1], int16_t{-200});

  LevelMeter m;
  std::vector<int16_t> t = {16384, 0, -16384, 0};
  m.add(t.data(), 2, 2);
  LevelMeter::Reading r = m.take();
  CHECK_EQ(r.channels, 2);
  CHECK_NEAR(r.peak_db[0], -6.02, 0.05);
  CHECK_NEAR(r.rms_db[0], -6.02, 0.05);
  CHECK_EQ(r.peak_db[1], -120.0);
  CHECK_EQ(m.take().frames, uint64_t(0));  // take() resets
  CHECK_NEAR(db_to_gain(-6.0206), 0.5, 1e-4);
  CHECK_EQ(db_to_gain(-200), 0.0);
}

void test_wav() {
  const std::string h = wav_header(44100, 2);
  CHECK_EQ(h.size(), size_t(44));
  CHECK(h.compare(0, 4, "RIFF") == 0);
  CHECK(h.compare(36, 4, "data") == 0);
  CHECK_EQ(static_cast<unsigned char>(h[24]) | (static_cast<unsigned char>(h[25]) << 8), 44100 & 0xffff);

  // A header is stripped, however it is split across reads; raw data passes as it is.
  const std::string wav = h + "ABCDEFGH";
  for (size_t cut = 1; cut < wav.size(); cut += 7) {
    WavStripper w;
    std::string out;
    w.feed(reinterpret_cast<const uint8_t*>(wav.data()), cut, &out);
    w.feed(reinterpret_cast<const uint8_t*>(wav.data()) + cut, wav.size() - cut, &out);
    CHECK_EQ(out, std::string("ABCDEFGH"));
  }
  WavStripper raw;
  std::string out;
  raw.feed(reinterpret_cast<const uint8_t*>("\x01\x02"), 2, &out);
  raw.feed(reinterpret_cast<const uint8_t*>("\x03\x04\x05"), 3, &out);
  CHECK_EQ(out, std::string("\x01\x02\x03\x04\x05"));
}

void test_playlists() {
  CHECK_EQ(playlist_first_url("[playlist]\nNumberOfEntries=2\nFile1=https://ice1.somafm.com/gs-128-mp3\nFile2=x\n"),
           std::string("https://ice1.somafm.com/gs-128-mp3"));
  CHECK_EQ(playlist_first_url("#EXTM3U\n#EXTINF:-1,Radio\r\nhttp://r.example/live.mp3\r\n"),
           std::string("http://r.example/live.mp3"));
  CHECK_EQ(playlist_first_url("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nhttp://a/b.m3u8\n"), std::string());
  CHECK(looks_like_playlist("https://somafm.com/groovesalad.pls"));
  CHECK(looks_like_playlist("http://x/live.M3U?token=1"));
  CHECK(!looks_like_playlist("http://x/live.mp3"));
  CHECK(!looks_like_playlist("http://x/index.m3u8"));

  std::string k, v;
  CHECK(parse_icy_line("ICY-META: StreamTitle='Artist - Song';StreamUrl='';", &k, &v));
  CHECK_EQ(k, std::string("title"));
  CHECK_EQ(v, std::string("Artist - Song"));
  CHECK(parse_icy_line("ICY-NAME: Groove Salad", &k, &v));
  CHECK_EQ(k, std::string("name"));
  CHECK(parse_icy_line("    icy-name        : Drone Zone", &k, &v));
  CHECK_EQ(v, std::string("Drone Zone"));
  CHECK(!parse_icy_line("    icy-br          : 128", &k, &v));
  CHECK(!parse_icy_line("Playing MPEG stream 1 of 1", &k, &v));

  const std::string hdrs = "HTTP/1.1 302 Found\r\nLocation: x\r\n\r\nHTTP/1.0 200 OK\r\nicy-metaint: 4\r\n\r\n";
  std::string title;
  size_t need = 0;
  CHECK(!icy_title_from(hdrs, "abc", &title, &need));
  CHECK_EQ(need, size_t(5));
  std::string body = "abcd";
  body.push_back(2);
  body += "StreamTitle='T1';";
  body.resize(4 + 1 + 32, '\0');
  CHECK(icy_title_from(hdrs, body, &title, &need));
  CHECK_EQ(title, std::string("T1"));
  CHECK(!icy_title_from("HTTP/1.1 200 OK\r\n\r\n", body, &title, &need));  // no ICY metadata
  CHECK_EQ(need, size_t(0));
}

void test_specs() {
  StreamSpec s;
  std::string err;
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"tone","freq":30000}})"), &s, &err));
  CHECK_EQ(s.source.freq, 24000.0);  // clamped to Nyquist
  CHECK_EQ(s.sink.type, std::string("none"));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"rate":12345})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"file:///etc/passwd"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://a b"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://x","decoder":"sh"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sink":{"type":"pipewire","target":"--foo"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sink":{"type":"alsa"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","monitor":true}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone","freq":"x"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"exec"}})"), &s, &err));
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"bluez_input.5C_E9_1E_22_40_01.2"},
      "sink":{"type":"pipewire","target":"bluez_output.F8_DF_15_0A_11_3C.1"},"rate":44100,"gain_db":99})"), &s, &err));
  CHECK_EQ(s.gain_db, 20.0);
  CHECK_EQ(default_label(s), std::string("capture bluez_input.5C_E9_1E_22_40_01.2 → bluez_output.F8_DF_15_0A_11_3C.1"));
  const json back = stream_spec_json(s);
  CHECK_EQ(back["source"]["target"].get<std::string>(), std::string("bluez_input.5C_E9_1E_22_40_01.2"));
  CHECK_EQ(back["rate"].get<int>(), 44100);

  // A media server's track carries its artist, album and length; repeat and shuffle round-trip.
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"url","repeat":"all","shuffle":true,"playlist":[
      {"url":"http://u/1.mp3","title":"One","artist":"Band","album":"LP","duration_ms":183000}]}})"), &s, &err));
  CHECK_EQ(s.source.playlist[0].artist, std::string("Band"));
  CHECK_EQ(s.source.playlist[0].duration_ms, 183000u);
  CHECK_EQ(stream_spec_json(s)["source"]["repeat"].get<std::string>(), std::string("all"));
  CHECK(stream_spec_json(s)["source"]["shuffle"].get<bool>());
  CHECK_EQ(stream_spec_json(s)["source"]["playlist"][0]["album"].get<std::string>(), std::string("LP"));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://u","repeat":"forever"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://u","shuffle":"yes"}})"), &s, &err));

  // A URL stream's playlist: its first item is the URL, given or not; a lone URL is a playlist of one.
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://r/radio","title":"Radio"}})"), &s, &err));
  CHECK_EQ(s.source.playlist.size(), size_t(1));
  CHECK_EQ(s.source.playlist[0].title, std::string("Radio"));
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"url","playlist":[
      {"url":"http://u/1.mp3","title":"One"},{"url":"http://u/2.mp3","title":"Two"}]}})"), &s, &err));
  CHECK_EQ(s.source.url, std::string("http://u/1.mp3"));
  CHECK_EQ(s.source.title, std::string("One"));
  CHECK_EQ(s.source.playlist.size(), size_t(2));
  CHECK_EQ(stream_spec_json(s)["source"]["playlist"][1]["title"].get<std::string>(), std::string("Two"));
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://u/0.mp3","playlist":[{"url":"http://u/1.mp3"}]}})"), &s, &err));
  CHECK_EQ(s.source.playlist.size(), size_t(2));  // the URL goes first
  CHECK_EQ(s.source.playlist[0].url, std::string("http://u/0.mp3"));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","playlist":[]}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","playlist":[{"url":"file:///x"}]}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url"}})"), &s, &err));

  CHECK(endpoint_id_ok("plughw:CARD=Loopback,DEV=0"));
  CHECK(endpoint_id_ok("hw:Loopback,0,0"));
  CHECK(!endpoint_id_ok("-D"));
  CHECK(!endpoint_id_ok("a b"));
  CHECK(!endpoint_id_ok("x;rm"));
}

void test_argv() {
  Tools t;
  t.pw_cat = "/usr/bin/pw-cat";
  t.aplay = "/usr/bin/aplay";
  t.arecord = "/usr/bin/arecord";
  t.ffmpeg = "/usr/bin/ffmpeg";
  std::vector<std::string> a;
  std::string err;
  SinkSpec k;
  k.type = "pipewire";
  k.target = "bluez_output.X.1";
  CHECK(sink_argv(k, 48000, 2, 60, "btbench-stream-1", "tone \"1 kHz\"", t, &a, &err));
  CHECK_EQ(a.back(), std::string("-"));
  CHECK_EQ(a[a.size() - 2], std::string("bluez_output.X.1"));
  // The description cannot break out of its property string.
  bool quoted = false;
  for (const std::string& x : a)
    if (x.find("tone 1 kHz") != std::string::npos && x.find("\\") == std::string::npos) quoted = true;
  CHECK(quoted);
  k.type = "alsa";
  k.target = "hw:Loopback,0,0";
  CHECK(sink_argv(k, 44100, 1, 100, "n", "d", t, &a, &err));
  CHECK_EQ(a[0], std::string("/usr/bin/aplay"));
  CHECK(std::find(a.begin(), a.end(), "--buffer-time=100000") != a.end());

  SourceSpec s;
  s.type = "url";
  s.url = "http://r/live.mp3";
  // No mpg123: auto falls back to ffmpeg.
  CHECK(decoder_argv(s, s.url, 48000, 2, t, &a, &err));
  CHECK_EQ(a[0], std::string("/usr/bin/ffmpeg"));
  t.mpg123 = "/usr/bin/mpg123";
  CHECK(decoder_argv(s, s.url, 44100, 1, t, &a, &err));
  CHECK_EQ(a[0], std::string("/usr/bin/mpg123"));
  CHECK(std::find(a.begin(), a.end(), "--mono") != a.end());
  CHECK_EQ(a.back(), s.url);
  // An AAC stream goes to ffmpeg when there is one.
  CHECK(decoder_argv(s, "http://r/live.aac", 44100, 2, t, &a, &err));
  CHECK_EQ(a[0], std::string("/usr/bin/ffmpeg"));
  Tools none;
  CHECK(!decoder_argv(s, s.url, 44100, 2, none, &a, &err));
  CHECK(err.find("mpg123") != std::string::npos);

  s.type = "capture";
  s.backend = "pipewire";
  s.target = "alsa_output.x";
  s.monitor = true;
  CHECK(capture_argv(s, 48000, 2, 50, "n-in", t, &a, &err));
  bool mon = false;
  for (const std::string& x : a)
    if (x.find("stream.capture.sink = true") != std::string::npos) mon = true;
  CHECK(mon);
}

void test_endpoints() {
  const json dump = json::parse(R"([
    {"id": 40, "type": "PipeWire:Interface:Node", "info": {"state": "running", "props": {
      "media.class": "Audio/Sink", "node.name": "bluez_output.F8_DF_15_0A_11_3C.1", "device.api": "bluez5",
      "node.description": "JBL Flip 5", "api.bluez5.address": "F8:DF:15:0A:11:3C", "api.bluez5.profile": "a2dp-sink",
      "api.bluez5.codec": "sbc", "object.serial": 120}}},
    {"id": 41, "type": "PipeWire:Interface:Node", "info": {"state": "suspended", "props": {
      "media.class": "Audio/Sink", "node.name": "alsa_output.platform-snd_aloop.0.analog-stereo",
      "device.api": "alsa", "alsa.id": "Loopback", "node.description": "Loopback Analog Stereo"}}},
    {"id": 42, "type": "PipeWire:Interface:Node", "info": {"props": {
      "media.class": "Audio/Source", "node.name": "bluez_input.5C_E9_1E_22_40_01.2", "device.api": "bluez5",
      "node.description": "Pixel 7"}}},
    {"id": 43, "type": "PipeWire:Interface:Node", "info": {"props": {
      "media.class": "Stream/Output/Audio", "node.name": "btbench-stream-1"}}},
    {"id": 45, "type": "PipeWire:Interface:Node", "info": {"state": "running", "props": {
      "media.class": "Stream/Output/Audio", "node.name": "bluez_input.A4_B1_C1_0B_28_E4.2", "device.api": "bluez5",
      "api.bluez5.address": "A4:B1:C1:0B:28:E4", "api.bluez5.profile": "a2dp-source", "api.bluez5.codec": "sbc",
      "node.description": "SMI-PC"}}},
    {"id": 44, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Video/Source", "node.name": "v4l2"}}},
    {"id": 0, "type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "default"}, "metadata": [
      {"subject": 0, "key": "default.configured.audio.sink", "value": {"name": "gone"}},
      {"subject": 0, "key": "default.audio.sink", "value": {"name": "bluez_output.F8_DF_15_0A_11_3C.1"}},
      {"subject": 0, "key": "default.audio.source", "value": "{\"name\": \"bluez_input.5C_E9_1E_22_40_01.2\"}"}]}
  ])");
  const json e = endpoints_from_pw_dump(dump);
  CHECK_EQ(e["sinks"].size(), size_t(2));
  CHECK_EQ(e["sources"].size(), size_t(2));  // the Pixel's source and SMI-PC's A2DP stream
  const json& jbl = e["sinks"][0];  // Bluetooth first
  CHECK_EQ(jbl["kind"].get<std::string>(), std::string("bluetooth"));
  CHECK_EQ(jbl["address"].get<std::string>(), std::string("F8:DF:15:0A:11:3C"));
  CHECK(jbl["default"].get<bool>());
  CHECK_EQ(jbl["serial"].get<std::string>(), std::string("120"));
  CHECK_EQ(e["sinks"][1]["kind"].get<std::string>(), std::string("loopback"));
  CHECK(!e["sinks"][1]["default"].get<bool>());
  const json& pix = e["sources"][0]["label"] == "Pixel 7" ? e["sources"][0] : e["sources"][1];
  CHECK_EQ(pix["address"].get<std::string>(), std::string("5C:E9:1E:22:40:01"));  // from the node name
  CHECK(pix["default"].get<bool>());  // the metadata value as a string, too
  CHECK(endpoints_from_pw_dump(json::object())["sinks"].empty());

  // Without PipeWire only the loopback card is left to play into and capture from.
  const json a = endpoints_from_alsa(true);
  CHECK_EQ(a["sinks"].size(), size_t(1));
  CHECK_EQ(a["sources"].size(), size_t(1));
  CHECK_EQ(a["sinks"][0]["id"].get<std::string>(), std::string("hw:Loopback,0,0"));
  CHECK_EQ(a["sinks"][0]["backend"].get<std::string>(), std::string("alsa"));
  CHECK_EQ(a["sources"][0]["id"].get<std::string>(), std::string("hw:Loopback,1,0"));
  for (const json& x : a["sinks"]) CHECK(endpoint_id_ok(x["id"].get<std::string>()));
  const json bare = endpoints_from_alsa(false);
  CHECK(bare["sinks"].empty() && bare["sources"].empty());
}

void test_pw_dump_parse() {
  // The dump, then an update that changes node 2 and removes node 3.
  const json d = parse_pw_dump(R"([{"id": 1, "info": {"a": 1}}, {"id": 2, "info": {"a": 2}}, {"id": 3, "info": {}}]
[{"id": 2, "info": {"a": 22}}, {"id": 3, "info": null}]
[{"id": 4, "info": {"a": 4)");  // a truncated tail
  CHECK_EQ(d.size(), size_t(2));
  CHECK_EQ(d[1]["info"]["a"].get<int>(), 22);
  CHECK(parse_pw_dump("").empty());
}

// The order a playlist plays in, with repeat and shuffle.
void test_play_order() {
  PlayOrder o(7);
  o.reset(3, 0, false);
  CHECK_EQ(o.next("off", false, false), 1);
  CHECK_EQ(o.next("off", false, false), 2);
  CHECK_EQ(o.next("off", false, false), -1);   // the end of the list
  CHECK_EQ(o.next("off", true, false), 2);     // next on the last track: it again
  CHECK_EQ(o.next("track", false, false), 2);  // repeat one: at its end, the same track
  CHECK_EQ(o.next("all", false, false), 0);    // repeat all: round again
  CHECK_EQ(o.previous("off"), 0);              // previous on the first: it again
  CHECK_EQ(o.previous("all"), 2);              // ... or, repeating all, the last
  o.go(1);
  CHECK_EQ(o.next("off", true, false), 2);
  o.grow(4, false);
  CHECK_EQ(o.next("off", false, false), 3);

  // Shuffled: the one playing first, then every other track once, then the end.
  o.reset(6, 4, true);
  CHECK_EQ(o.order().front(), 4);
  std::vector<int> seen{4};
  for (int t; (t = o.next("off", false, true)) >= 0;) seen.push_back(t);
  std::sort(seen.begin(), seen.end());
  CHECK(seen == (std::vector<int>{0, 1, 2, 3, 4, 5}));
  // Repeating all, a new round does not start with the track that just ended.
  const int last = o.order().back();
  CHECK(o.next("all", false, true) != last);
  CHECK_EQ(o.order().size(), size_t(6));
  // A track enqueued while shuffled comes later, not before the one playing.
  o.reset(2, 1, true);
  o.grow(3, true);
  CHECK_EQ(o.order().front(), 1);
  CHECK_EQ(o.order().size(), size_t(3));
  CHECK(PlayOrder().next("off", true, false) == -1);  // nothing to play
}

void test_cards() {
  const json dump = json::parse(R"J([
    {"id": 58, "type": "PipeWire:Interface:Device", "info": {
      "props": {"device.api": "bluez5", "device.name": "bluez_card.B8_27_EB_8F_D1_56", "device.description": "btbench"},
      "params": {
        "EnumProfile": [
          {"index": 0, "name": "off", "description": "Off", "available": "yes"},
          {"index": 1, "name": "audio-gateway", "description": "Audio Gateway (A2DP Source & HSP/HFP AG)", "available": "yes"},
          {"index": 6, "name": "a2dp-sink-sbc_xq", "description": "High Fidelity Playback (A2DP Sink, codec SBC-XQ)", "available": "yes"},
          {"index": 20, "name": "a2dp-duplex-opus_05_duplex", "description": "High Fidelity Duplex (A2DP Source/Sink, codec Opus 05 Duplex)", "available": "yes"},
          {"index": 196865, "name": "headset-head-unit", "description": "Headset Head Unit (HSP/HFP, codec mSBC)", "available": "no"}],
        "Profile": [{"index": 6, "name": "a2dp-sink-sbc_xq", "description": "High Fidelity Playback (A2DP Sink, codec SBC-XQ)"}]}}},
    {"id": 41, "type": "PipeWire:Interface:Device", "info": {"props": {"device.api": "alsa", "device.name": "alsa_card.x"}}}
  ])J");
  const json c = cards_from_pw_dump(dump);
  CHECK_EQ(c["devices"].size(), size_t(1));  // ALSA cards are not the picker's
  const json& d = c["devices"][0];
  CHECK_EQ(d["id"].get<int>(), 58);
  CHECK_EQ(d["address"].get<std::string>(), std::string("B8:27:EB:8F:D1:56"));
  CHECK_EQ(d["label"].get<std::string>(), std::string("btbench"));
  CHECK_EQ(d["active"]["codec"].get<std::string>(), std::string("SBC-XQ"));
  CHECK_EQ(d["active"]["kind"].get<std::string>(), std::string("a2dp"));
  CHECK_EQ(d["profiles"].size(), size_t(5));
  CHECK_EQ(d["profiles"][1]["kind"].get<std::string>(), std::string("gateway"));
  CHECK_EQ(d["profiles"][1]["codec"].get<std::string>(), std::string());
  CHECK_EQ(d["profiles"][3]["kind"].get<std::string>(), std::string("a2dp-duplex"));
  CHECK_EQ(d["profiles"][3]["codec"].get<std::string>(), std::string("Opus 05 Duplex"));
  CHECK(!d["profiles"][4]["available"].get<bool>());
  CHECK_EQ(d["profiles"][4]["kind"].get<std::string>(), std::string("headset"));
  CHECK_EQ(profile_codec("Off"), std::string());
}

void test_xml() {
  const XmlNode d = parse_xml(R"(<?xml version="1.0"?><!-- c --><a:root x='1' y="&lt;2&gt;">
      <b>one &amp; two &#233;&#x41;</b><c/><b k=v>three<![CDATA[<raw>]]></b></a:root>)");
  const XmlNode* root = d.child("root");
  CHECK(root != nullptr);
  if (!root) return;
  CHECK_EQ(root->attrs.at("y"), std::string("<2>"));
  CHECK_EQ(root->all("b").size(), size_t(2));
  CHECK_EQ(root->child_text("b"), std::string("one & two \xc3\xa9" "A"));
  CHECK_EQ(root->all("b")[1]->text, std::string("three<raw>"));
  CHECK(root->child("c") != nullptr);
  // Truncated input: what was there, no exception.
  const XmlNode t = parse_xml("<a><b>x</b><c attr=\"unterminated");
  CHECK(t.find("b") != nullptr);
  CHECK_EQ(xml_escape("<a&'\">"), std::string("&lt;a&amp;&apos;&quot;&gt;"));
}

void test_upnp() {
  SsdpReply r;
  CHECK(parse_ssdp_reply("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nlocation: http://192.168.1.5:8200/rootDesc.xml\r\n"
                         "USN: uuid:4d696e69-444c-164e-9d41-b827eb507b22::urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n", &r));
  CHECK_EQ(r.location, std::string("http://192.168.1.5:8200/rootDesc.xml"));
  CHECK(!parse_ssdp_reply("NOTIFY * HTTP/1.1\r\nLOCATION: x\r\n\r\n", &r));
  CHECK(ssdp_msearch("ssdp:all", 2).find("MX: 2\r\n") != std::string::npos);

  CHECK_EQ(resolve_url("http://h:1/a/b.xml", "/ctl"), std::string("http://h:1/ctl"));
  CHECK_EQ(resolve_url("http://h:1/a/b.xml", "ctl/cd"), std::string("http://h:1/a/ctl/cd"));
  CHECK_EQ(resolve_url("http://h:1/a/b.xml", "http://o/x"), std::string("http://o/x"));
  std::string origin, path;
  CHECK(split_url("http://h:8200/a?b", &origin, &path));
  CHECK_EQ(origin, std::string("http://h:8200"));
  CHECK_EQ(path, std::string("/a?b"));
  CHECK(!split_url("ftp://h/x", &origin, &path));

  MediaServer s;
  CHECK(parse_description(R"(<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
      <deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType><friendlyName>NAS</friendlyName><UDN>uuid:nas</UDN>
      <deviceList><device><deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
        <friendlyName>NAS Music</friendlyName><manufacturer>Acme</manufacturer><UDN>uuid:ms</UDN>
        <iconList><icon><mimetype>image/jpeg</mimetype><url>/i.jpg</url></icon><icon><mimetype>image/png</mimetype><url>/i.png</url></icon></iconList>
        <serviceList><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/cm</controlURL></service>
        <service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType><controlURL>ctl/ContentDir</controlURL></service></serviceList>
      </device></deviceList></device></root>)", "http://10.0.0.2:8200/desc/root.xml", &s));
  CHECK_EQ(s.name, std::string("NAS Music"));
  CHECK_EQ(s.id, std::string("uuid:ms"));
  CHECK_EQ(s.control_url, std::string("http://10.0.0.2:8200/desc/ctl/ContentDir"));
  CHECK_EQ(s.icon_url, std::string("http://10.0.0.2:8200/i.png"));
  CHECK(!parse_description("<root><device><serviceList/></device></root>", "http://x/", &s));

  const std::string body = soap_browse_body("64$1", false, 0, 50);
  CHECK(body.find("<ObjectID>64$1</ObjectID>") != std::string::npos);
  CHECK(body.find("BrowseDirectChildren") != std::string::npos);

  const std::string reply = R"(<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>
    <u:BrowseResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1"><Result>&lt;DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"&gt;
&lt;container id="64$1" parentID="64" childCount="12"&gt;&lt;dc:title&gt;Albums&lt;/dc:title&gt;&lt;upnp:class&gt;object.container.storageFolder&lt;/upnp:class&gt;&lt;/container&gt;
&lt;item id="64$2$0" parentID="64$2"&gt;&lt;dc:title&gt;Song &amp;amp; Dance&lt;/dc:title&gt;&lt;dc:creator&gt;Band&lt;/dc:creator&gt;&lt;upnp:album&gt;LP&lt;/upnp:album&gt;
&lt;res protocolInfo="http-get:*:image/jpeg:*"&gt;http://10.0.0.2:8200/art.jpg&lt;/res&gt;
&lt;res protocolInfo="http-get:*:audio/flac:*" duration="0:03:25.500" size="31337"&gt;http://10.0.0.2:8200/MediaItems/22.flac&lt;/res&gt;&lt;/item&gt;
&lt;/DIDL-Lite&gt;</Result><NumberReturned>2</NumberReturned><TotalMatches>2</TotalMatches><UpdateID>1</UpdateID></u:BrowseResponse></s:Body></s:Envelope>)";
  std::string didl, fault;
  unsigned ret = 0, total = 0;
  CHECK(parse_browse_response(reply, &didl, &ret, &total, &fault));
  CHECK_EQ(ret, 2u);
  const json l = parse_didl(didl);
  CHECK_EQ(l["containers"].size(), size_t(1));
  CHECK_EQ(l["containers"][0]["child_count"].get<int>(), 12);
  CHECK_EQ(l["items"].size(), size_t(1));
  const json& it = l["items"][0];
  CHECK_EQ(it["title"].get<std::string>(), std::string("Song & Dance"));
  CHECK_EQ(it["artist"].get<std::string>(), std::string("Band"));
  CHECK_EQ(it["url"].get<std::string>(), std::string("http://10.0.0.2:8200/MediaItems/22.flac"));  // the audio res
  CHECK_EQ(it["mime"].get<std::string>(), std::string("audio/flac"));
  CHECK_NEAR(it["duration_s"].get<double>(), 205.5, 1e-9);
  CHECK_EQ(it["size"].get<int>(), 31337);
  // No albumArtURI: the image resource is the cover.
  CHECK_EQ(it["art"].get<std::string>(), std::string("http://10.0.0.2:8200/art.jpg"));
  CHECK_EQ(it["thumb"].get<std::string>(), std::string("http://10.0.0.2:8200/art.jpg"));

  // MinimServer's album and track: the album artist, the year, sized covers (smallest for the row,
  // largest for the picture), the track number and the resource's format.
  const json m = parse_didl(R"(<DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/" xmlns:dlna="urn:schemas-dlna-org:metadata-1-0/">
<container id="0$albums$*a2" childCount="14"><dc:title>3Teeth</dc:title><upnp:genre>Industrial</upnp:genre><dc:date>2014-01-01</dc:date>
<upnp:artist>Guest</upnp:artist><upnp:artist role="AlbumArtist">3TEETH</upnp:artist>
<upnp:albumArtURI dlna:profileID="JPEG_LRG">http://s/big.jpg</upnp:albumArtURI><upnp:albumArtURI dlna:profileID="JPEG_TN">http://s/tn.jpg</upnp:albumArtURI>
<upnp:class>object.container.album.musicAlbum</upnp:class></container>
<item id="0$albums$*a2$*i1"><dc:title>Nihil</dc:title><upnp:artist>3TEETH</upnp:artist><upnp:originalTrackNumber>1</upnp:originalTrackNumber><dc:date>2014</dc:date>
<upnp:albumArtURI dlna:profileID="JPEG_MED">http://s/med.jpg</upnp:albumArtURI>
<res duration="0:04:02" bitsPerSample="24" sampleFrequency="96000" nrAudioChannels="2" protocolInfo="http-get:*:audio/x-flac:*">http://s/1.flac</res>
<upnp:class>object.item.audioItem.musicTrack</upnp:class></item></DIDL-Lite>)");
  const json& al = m["containers"][0];
  CHECK_EQ(al["artist"].get<std::string>(), std::string("3TEETH"));
  CHECK_EQ(al["year"].get<std::string>(), std::string("2014"));
  CHECK_EQ(al["genre"].get<std::string>(), std::string("Industrial"));
  CHECK_EQ(al["thumb"].get<std::string>(), std::string("http://s/tn.jpg"));
  CHECK_EQ(al["art"].get<std::string>(), std::string("http://s/big.jpg"));
  const json& tr = m["items"][0];
  CHECK_EQ(tr["track"].get<int>(), 1);
  CHECK_EQ(tr["rate"].get<int>(), 96000);
  CHECK_EQ(tr["bits"].get<int>(), 24);
  CHECK_EQ(tr["channels"].get<int>(), 2);
  CHECK_EQ(tr["thumb"].get<std::string>(), std::string("http://s/med.jpg"));
  CHECK_EQ(tr["year"].get<std::string>(), std::string("2014"));

  CHECK(!parse_browse_response(R"(<s:Envelope><s:Body><s:Fault><faultstring>UPnPError</faultstring><detail><UPnPError>
      <errorCode>701</errorCode><errorDescription>No such object</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>)",
      &didl, &ret, &total, &fault));
  CHECK(fault.find("No such object") != std::string::npos);
  CHECK(fault.find("701") != std::string::npos);
  CHECK_NEAR(parse_duration("03:25"), 205.0, 1e-9);
  CHECK_EQ(parse_duration("x"), -1.0);
}

}  // namespace

int main() {
  test_tone();
  test_sweep_noise();
  test_gain_meter();
  test_wav();
  test_playlists();
  test_specs();
  test_argv();
  test_endpoints();
  test_pw_dump_parse();
  test_play_order();
  test_cards();
  test_xml();
  test_upnp();
  return report("test_audio");
}
