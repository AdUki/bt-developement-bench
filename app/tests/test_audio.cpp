// The audio streams' decisions: the generators' levels and continuity, the meter, the WAV
// framing, what a stream body may say and the command lines it becomes, the endpoint lists made
// from pw-dump and from BlueZ's transports, and the UPnP parsers. The pump's sinks, and a
// left/right pair's two captures, are exercised against fake aplay/arecord/pw-cat processes
// (test_engine_sinks); the rest of the pumping with `make run` against the desktop's PipeWire and
// a radio stream.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>

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

  CHECK(parse_icy_line("ICY-META: StreamTitle='Leading Britain's Conversation';StreamUrl='';", &k, &v));
  CHECK_EQ(v, std::string("Leading Britain's Conversation"));

  CHECK_EQ(icy_field("StreamTitle='A - B';StreamUrl='http://img.example/c/1.jpg';", "StreamUrl"),
           std::string("http://img.example/c/1.jpg"));
  CHECK_EQ(icy_field("StreamTitle='A - B';", "StreamUrl"), std::string());

  const std::string hdrs = "HTTP/1.1 302 Found\r\nicy-metaint: 9\r\nLocation: x\r\n\r\n"
                           "HTTP/1.0 200 OK\r\nicy-metaint: 4\r\nicy-description: Eclectic\r\n\r\n";
  CHECK_EQ(http_header(hdrs, "icy-metaint"), std::string("4"));  // the last response's
  CHECK_EQ(http_header(hdrs, "icy-description"), std::string("Eclectic"));
  CHECK_EQ(http_header(hdrs, "icy-name"), std::string());
  std::string meta;
  size_t need = 0;
  CHECK(!icy_meta_from(hdrs, "abc", &meta, &need));
  CHECK_EQ(need, size_t(5));
  std::string body = "abcd";
  body.push_back(3);
  body += "StreamTitle='T1';StreamUrl='http://x/a.png';";
  body.resize(4 + 1 + 48, '\0');
  CHECK(icy_meta_from(hdrs, body, &meta, &need));
  CHECK_EQ(icy_field(meta, "StreamTitle"), std::string("T1"));
  CHECK_EQ(meta.find('\0'), std::string::npos);
  CHECK(!icy_meta_from("HTTP/1.1 200 OK\r\n\r\n", body, &meta, &need));  // no ICY metadata
  CHECK_EQ(need, size_t(0));

  // Frames that chain: MP3 (MPEG-1 layer III, 128 kb/s, 44.1 kHz: 417 bytes) and ADTS (200 bytes),
  // each payload full of sync-like bytes of the other kind.
  auto frames = [](const std::string& hdr, size_t len, const std::string& noise) {
    std::string f = hdr;
    while (f.size() < len) f += noise;
    f.resize(len);
    return f + f + f + f + f;
  };
  const std::string mp3 = frames(std::string("\xff\xfb\x90\x64", 4), 417, std::string("\xff\xf1\x50\x80\x00", 5));
  const std::string aac = frames(std::string("\xff\xf1\x50\x80\x19\x1f\xfc", 7), 200, std::string("\xff\xfb\x90\x00\x11", 5));
  CHECK_EQ(sniff_audio("audio/mpeg", aac), std::string("other"));  // Shoutcast's AAC
  CHECK_EQ(sniff_audio("audio/aac", mp3), std::string("mp3"));
  CHECK_EQ(sniff_audio("audio/aac", aac.substr(3)), std::string("other"));  // starting mid-frame
  CHECK_EQ(sniff_audio("", "OggS\0\x02"), std::string("other"));
  CHECK_EQ(sniff_audio("", "ID3\x04"), std::string("mp3"));
  CHECK_EQ(sniff_audio("audio/aacp; charset=x", "??"), std::string("other"));
  CHECK_EQ(sniff_audio("audio/mpeg", ""), std::string("mp3"));
  CHECK_EQ(sniff_audio("text/html", "<html>"), std::string());

  CHECK(url_is_image("http://img.radioparadise.com/covers/l/7152.jpg"));
  CHECK(url_is_image("https://somafm.com/logos/512/groovesalad512.JPG?v=2"));
  CHECK(!url_is_image("https://listenapi.planetradio.co.uk/api9.2/eventdata/420431971"));
  CHECK(!url_is_image("file:///etc/a.png"));
  CHECK_EQ(image_url_in_json(R"({"eventId": 1, "eventImageUrlSmall": "https://m/t/160x160/1.jpg",
    "eventImageUrl": "https://m/t/1.jpg", "eventSongTitle": "x"})"), std::string("https://m/t/1.jpg"));
  CHECK_EQ(image_url_in_json(R"({"now": [{"image_thumb": "http://a/1.png"}]})"), std::string("http://a/1.png"));
  CHECK_EQ(image_url_in_json(R"({"image": "/relative.png"})"), std::string());
  CHECK_EQ(image_url_in_json("<html>"), std::string());
}

void test_specs() {
  StreamSpec s;
  std::string err;
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"tone","freq":30000}})"), &s, &err));
  CHECK_EQ(s.source.freq, 24000.0);  // clamped to Nyquist
  CHECK(s.sinks.empty());
  CHECK(stream_spec_json(s)["sink"].is_null());
  CHECK(stream_spec_json(s)["sinks"].empty());
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"rate":12345})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"file:///etc/passwd"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://a b"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"url","url":"http://x","decoder":"sh"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sink":{"type":"pipewire","target":"--foo"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sink":{"type":"alsa"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","monitor":true}})"), &s, &err));
  // A left and a right sink: one channel each of a stereo stream (a broadcast's two BISes).
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[{"target":"a","channel":"left"},{"target":"b","channel":"right"}]})"), &s, &err));
  CHECK_EQ(s.sinks.size(), 2u);
  CHECK_EQ(s.sinks[1].channel, std::string("right"));
  CHECK_EQ(stream_spec_json(s)["sinks"][0]["channel"], json("left"));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"channels":1,"sinks":[{"target":"a","channel":"left"}]})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[{"target":"a","channel":"middle"}]})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone","freq":"x"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"exec"}})"), &s, &err));
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"bluez_input.5C_E9_1E_22_40_01.2"},
      "sink":{"type":"pipewire","target":"bluez_output.F8_DF_15_0A_11_3C.1"},"rate":44100,"gain_db":99})"), &s, &err));
  CHECK_EQ(s.gain_db, 20.0);
  CHECK_EQ(default_label(s), std::string("capture bluez_input.5C_E9_1E_22_40_01.2 → bluez_output.F8_DF_15_0A_11_3C.1"));
  const json back = stream_spec_json(s);
  CHECK_EQ(back["source"]["target"].get<std::string>(), std::string("bluez_input.5C_E9_1E_22_40_01.2"));
  CHECK_EQ(back["rate"].get<int>(), 44100);
  CHECK_EQ(back["sink"]["target"].get<std::string>(), std::string("bluez_output.F8_DF_15_0A_11_3C.1"));
  CHECK(back["source"]["target_right"].is_null());

  // A left/right pair: two PipeWire sources, a stereo stream; it round-trips.
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"bluez_input.big0.bis1",
      "target_right":"bluez_input.big0.bis2"}})"), &s, &err));
  CHECK_EQ(s.source.target_right, std::string("bluez_input.big0.bis2"));
  CHECK_EQ(default_label(s), std::string("capture bluez_input.big0.bis1 + bluez_input.big0.bis2 (L/R) → listeners"));
  StreamSpec again;
  CHECK(stream_spec_from_json(stream_spec_json(s), &again, &err));
  CHECK_EQ(again.source.target_right, std::string("bluez_input.big0.bis2"));
  CHECK_EQ(stream_spec_json(again)["source"]["target_right"].get<std::string>(), std::string("bluez_input.big0.bis2"));
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"a","target_right":null}})"), &s, &err));
  CHECK(s.source.target_right.empty());
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"a","target_right":"-x"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"a","target_right":"b c"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"a","target_right":5}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target_right":"b"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","backend":"alsa","target":"hw:A",
      "target_right":"hw:B"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"a","target_right":"b",
      "monitor":true}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"capture","target":"a","target_right":"b"},
      "channels":1})"), &s, &err));
  CHECK(err.find("channels") != std::string::npos);

  // Several sinks: each once, none left out; "sink" in the answer is the first.
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[
      {"type":"pipewire","target":"bluez_output.A.1"},{"type":"none"},{"type":"pipewire","target":"bluez_output.B.1"},
      {"type":"pipewire","target":"bluez_output.A.1"}]})"), &s, &err));
  CHECK_EQ(s.sinks.size(), size_t(2));
  CHECK_EQ(s.sinks[1].target, std::string("bluez_output.B.1"));
  CHECK_EQ(default_label(s), std::string("tone 1 kHz → bluez_output.A.1 + bluez_output.B.1"));
  CHECK_EQ(stream_spec_json(s)["sinks"].size(), size_t(2));
  CHECK_EQ(stream_spec_json(s)["sink"]["target"].get<std::string>(), std::string("bluez_output.A.1"));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":{"type":"pipewire"}})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[null]})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[{"type":"pipewire","target":"-x"}]})"), &s, &err));
  CHECK(!stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[{"target":"a"},{"target":"b"},
      {"target":"c"},{"target":"d"},{"target":"e"}]})"), &s, &err));  // kMaxSinks
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"tone"},"sinks":[]})"), &s, &err));
  CHECK(s.sinks.empty());
  CHECK(is_local(SourceSpec{}));
  SourceSpec cap;
  cap.type = "capture";
  CHECK(!is_local(cap));

  // A media server's track carries its artist, album and length; repeat and shuffle round-trip.
  CHECK(stream_spec_from_json(json::parse(R"({"source":{"type":"url","repeat":"all","shuffle":true,"playlist":[
      {"url":"http://u/1.mp3","title":"One","artist":"Band","album":"LP","duration_ms":183000,"art":"http://u/1.jpg"},
      {"url":"http://u/2.mp3","title":"Two","art":"cover.jpg"}]}})"), &s, &err));
  CHECK_EQ(s.source.playlist[0].artist, std::string("Band"));
  // Its cover, kept for a Bluetooth remote; one that is no URL is dropped.
  CHECK_EQ(stream_spec_json(s)["source"]["playlist"][0]["art"].get<std::string>(), std::string("http://u/1.jpg"));
  CHECK_EQ(s.source.playlist[1].art, std::string());
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
  // A sink named is that one only: no falling back to the default output when it is gone.
  CHECK(std::any_of(a.begin(), a.end(), [](const std::string& x) { return x.find("node.dont-fallback = true") != std::string::npos; }));
  k.target = "";
  CHECK(sink_argv(k, 48000, 2, 60, "n", "d", t, &a, &err));
  CHECK(std::none_of(a.begin(), a.end(), [](const std::string& x) { return x.find("dont-fallback") != std::string::npos; }));
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
  // A URL that does not say, sniffed: AAC to ffmpeg, MP3 to mpg123; unknown, mpg123 as before.
  CHECK(decoder_argv(s, "http://r/aac/96", 44100, 2, t, &a, &err, "other"));
  CHECK_EQ(a[0], std::string("/usr/bin/ffmpeg"));
  // https: ffmpeg reads it from curl (an image's ffmpeg has no TLS); the URL is an argument.
  t.curl = "/usr/bin/curl";
  CHECK(decoder_argv(s, "https://r/aac/96?a=$(x)", 44100, 2, t, &a, &err, "other"));
  CHECK_EQ(a[0], std::string("/bin/sh"));
  CHECK_EQ(a[a.size() - 2], std::string("https://r/aac/96?a=$(x)"));
  CHECK(a[2].find("$(x)") == std::string::npos);
  t.curl.clear();
  CHECK(decoder_argv(s, "https://r/live", 44100, 2, t, &a, &err, "mp3"));
  CHECK_EQ(a[0], std::string("/usr/bin/mpg123"));
  CHECK(decoder_argv(s, "https://r/live", 44100, 2, t, &a, &err));
  CHECK_EQ(a[0], std::string("/usr/bin/mpg123"));
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

  // A pair is two mono captures, each of its own target and node; capture_argv ignores target_right.
  const auto arg_after = [](const std::vector<std::string>& v, const std::string& k) {
    const auto it = std::find(v.begin(), v.end(), k);
    return it == v.end() || it + 1 == v.end() ? std::string() : *(it + 1);
  };
  const auto has = [](const std::vector<std::string>& v, const std::string& part) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& x) { return x.find(part) != std::string::npos; });
  };
  s.monitor = false;
  s.target = "bluez_input.l";
  s.target_right = "bluez_input.r";
  CHECK(capture_argv(s, 48000, 1, 50, "btbench-stream-2-in", t, &a, &err));
  CHECK_EQ(arg_after(a, "--channels"), std::string("1"));
  CHECK_EQ(arg_after(a, "--target"), std::string("bluez_input.l"));
  CHECK(has(a, "node.name = \"btbench-stream-2-in\""));
  CHECK(!has(a, "bluez_input.r"));
  SourceSpec r = s;
  r.target = r.target_right;
  CHECK(capture_argv(r, 48000, 1, 50, "btbench-stream-2-in-r", t, &a, &err));
  CHECK_EQ(arg_after(a, "--channels"), std::string("1"));
  CHECK_EQ(arg_after(a, "--target"), std::string("bluez_input.r"));
  CHECK(has(a, "node.name = \"btbench-stream-2-in-r\""));
  CHECK(has(a, "node.dont-fallback = true"));
}

std::string mono_bytes(std::initializer_list<int16_t> v) {
  std::string s;
  for (const int16_t x : v) s.append(reinterpret_cast<const char*>(&x), sizeof(x));
  return s;
}

void test_pair_mix() {
  // The right channel from the queue's front; silence for what it lacks.
  const int16_t left[4] = {1, 2, 3, 4};
  int16_t out[8];
  std::string q = mono_bytes({10, 20});
  PairMix m = interleave_pair(left, 4, &q, 100, out);
  const int16_t want[8] = {1, 10, 2, 20, 3, 0, 4, 0};
  CHECK(std::equal(out, out + 8, want));
  CHECK_EQ(m.padded, size_t(2));
  CHECK_EQ(m.dropped, size_t(0));
  CHECK(q.empty());

  // Enough queued: none padded, the rest kept for the next chunk.
  q = mono_bytes({10, 20, 30, 40, 50, 60});
  m = interleave_pair(left, 4, &q, 100, out);
  CHECK_EQ(out[7], int16_t(40));
  CHECK_EQ(m.padded, size_t(0));
  CHECK_EQ(q, mono_bytes({50, 60}));

  // Over the limit: cut to the newest chunk's worth (here 2), not to the limit.
  q = mono_bytes({10, 20, 30, 40, 50, 60, 70, 80, 90, 100});
  m = interleave_pair(left, 2, &q, 5, out);
  CHECK_EQ(out[1], int16_t(10));
  CHECK_EQ(out[3], int16_t(20));
  CHECK_EQ(m.dropped, size_t(6));
  CHECK_EQ(q, mono_bytes({90, 100}));

  // Half a sample read stays at the end, still the start of the next one.
  q = mono_bytes({10, 20, 30, 40, 50, 60, 70}) + std::string(1, '\x7f');
  CHECK_EQ(drop_backlog(&q, 5, 2), size_t(5));
  CHECK_EQ(q, mono_bytes({60, 70}) + std::string(1, '\x7f'));
  CHECK_EQ(drop_backlog(&q, 5, 2), size_t(0));
  q = std::string(1, '\x7f');
  m = interleave_pair(left, 1, &q, 5, out);
  CHECK_EQ(out[1], int16_t(0));
  CHECK_EQ(q.size(), size_t(1));
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
  CHECK(jbl["adapter"].is_null());  // no device object in this dump

  // One device connected on two adapters: two nodes of one name, told apart by the adapter of
  // their device and targeted by serial.
  const json two = json::parse(R"([
    {"id": 63, "type": "PipeWire:Interface:Device", "info": {"props": {"device.name": "bluez_card.B8_27_EB_8F_D1_56",
      "api.bluez5.path": "/org/bluez/hci1/dev_B8_27_EB_8F_D1_56"}}},
    {"id": 79, "type": "PipeWire:Interface:Device", "info": {"props": {"device.name": "bluez_card.B8_27_EB_8F_D1_56",
      "api.bluez5.path": "/org/bluez/hci0/dev_B8_27_EB_8F_D1_56"}}},
    {"id": 71, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink",
      "node.name": "bluez_output.B8_27_EB_8F_D1_56.1", "device.api": "bluez5", "device.id": 63,
      "api.bluez5.address": "B8:27:EB:8F:D1:56", "node.description": "btbench-702ea9", "object.serial": 312}}},
    {"id": 78, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink",
      "node.name": "bluez_output.B8_27_EB_8F_D1_56.1", "device.api": "bluez5", "device.id": 79,
      "api.bluez5.address": "B8:27:EB:8F:D1:56", "node.description": "btbench-702ea9", "object.serial": 327}}},
    {"id": 80, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink",
      "node.name": "bluez_output.10_2C_6B_A2_0A_0F.1", "device.api": "bluez5",
      "api.bluez5.address": "10:2C:6B:A2:0A:0F", "node.description": "btbench-712d0b", "object.serial": 330}}}
  ])");
  const json t = endpoints_from_pw_dump(two)["sinks"];
  CHECK_EQ(t.size(), size_t(3));
  CHECK_EQ(t[0]["label"].get<std::string>(), std::string("btbench-702ea9 (hci0)"));
  CHECK_EQ(t[0]["id"].get<std::string>(), std::string("327"));
  CHECK_EQ(t[0]["adapter"].get<std::string>(), std::string("hci0"));
  CHECK_EQ(t[1]["label"].get<std::string>(), std::string("btbench-702ea9 (hci1)"));
  CHECK_EQ(t[1]["id"].get<std::string>(), std::string("312"));
  CHECK_EQ(t[2]["id"].get<std::string>(), std::string("bluez_output.10_2C_6B_A2_0A_0F.1"));  // a name of its own
  CHECK(endpoint_id_ok("312"));

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

// The order a playlist plays in, with repeat and shuffle: shuffled, the list itself is in the
// order it plays.
std::vector<PlaylistItem> tracks(int n) {
  std::vector<PlaylistItem> l(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) l[static_cast<size_t>(i)].title = std::to_string(i);
  return l;
}
std::string titles(const std::vector<PlaylistItem>& l) {
  std::string s;
  for (const PlaylistItem& it : l) s += it.title;
  return s;
}

void test_play_order() {
  PlayOrder o(7);
  std::vector<PlaylistItem> l = tracks(3);
  o.reset(l, false);
  CHECK(l[0].id && l[1].id && l[2].id && l[0].id != l[1].id);
  CHECK_EQ(o.next(l, "off", false), 1);
  CHECK_EQ(o.next(l, "off", false), 2);
  CHECK_EQ(o.next(l, "off", false), -1);   // the end of the list
  CHECK_EQ(o.next(l, "off", true), 2);     // next on the last track: it again
  CHECK_EQ(o.next(l, "track", false), 2);  // repeat one: at its end, the same track
  CHECK_EQ(o.next(l, "all", false), 0);    // repeat all: round again
  CHECK_EQ(o.previous(l, "off"), 0);       // previous on the first: it again
  CHECK_EQ(o.previous(l, "all"), 2);       // ... or, repeating all, the last
  o.go(l, 1);
  CHECK_EQ(o.next(l, "off", true), 2);
  o.insert(l, tracks(1), false);
  CHECK_EQ(o.next(l, "off", false), 3);
  CHECK_EQ(titles(l), std::string("0120"));

  // Added to play next: right after the one playing.
  o.go(l, 1);
  std::vector<PlaylistItem> x = tracks(1);
  x[0].title = "x";
  o.insert(l, x, true);
  CHECK_EQ(titles(l), std::string("01x20"));
  CHECK_EQ(o.next(l, "off", true), 2);
  // Taken out: any but the one playing.
  CHECK(!o.remove(l, l[2].id));
  CHECK(o.remove(l, l[0].id));
  CHECK(!o.remove(l, 999));
  CHECK_EQ(titles(l), std::string("1x20"));
  CHECK_EQ(o.at(l), 1);

  // Shuffled: the one playing first, then every other track once in the list's own order, then
  // the end; off again, back as listed with the one playing where it was.
  l = tracks(6);
  o.reset(l, false);
  o.go(l, 4);
  o.shuffle(l, true);
  CHECK_EQ(l.front().title, std::string("4"));
  CHECK_EQ(o.at(l), 0);
  std::string order = titles(l);
  std::sort(order.begin(), order.end());
  CHECK_EQ(order, std::string("012345"));
  const std::string shuffled = titles(l);
  std::string played = l.front().title;
  for (int t; (t = o.next(l, "off", false)) >= 0;) played += l[static_cast<size_t>(t)].title;
  CHECK_EQ(played, shuffled);
  // Added while shuffled, to play next: after the one playing in both orders.
  std::vector<PlaylistItem> y = tracks(1);
  y[0].title = "y";
  o.insert(l, y, true);
  CHECK_EQ(l.back().title, std::string("y"));  // the last track plays
  o.shuffle(l, false);
  std::string listed = "012345";
  listed.insert(listed.find(shuffled.back()) + 1, "y");
  CHECK_EQ(titles(l), listed);
  CHECK_EQ(l[static_cast<size_t>(o.at(l))].title, std::string(1, shuffled.back()));

  // Repeating all, a new round is a new order that does not start with the track just played.
  l = tracks(6);
  o.reset(l, true);
  CHECK_EQ(l.front().title, std::string("0"));
  while (o.next(l, "off", false) >= 0) {}
  const uint32_t last = o.current();
  CHECK_EQ(o.next(l, "all", false), 0);
  CHECK(l.front().id != last);
  CHECK_EQ(l.size(), size_t(6));

  std::vector<PlaylistItem> none;
  CHECK(PlayOrder().next(none, "off", true) == -1);  // nothing to play
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

// ---- the pump's sinks, against fake processes -----------------------------------------------------

// aplay as the pump runs it ("-D <target> ... -r <rate> -c <channels>"): takes the audio at the
// rate it says, as a sound card would, into <dir>/<target>; "hw:stall" takes none.
const char* kFakeAplay = R"(#!/usr/bin/env python3
import os, sys, time
a = sys.argv
t = a[a.index('-D') + 1]
if t.startswith('hw:stall'):
    time.sleep(60)
    sys.exit(0)
per_s = int(a[a.index('-r') + 1]) * int(a[a.index('-c') + 1]) * 2
out = open(os.path.join(os.path.dirname(sys.argv[0]), '..', t), 'wb', buffering=0)
t0, n = time.monotonic(), 0
while True:
    room = int((time.monotonic() - t0) * per_s) - n
    if room < 4096:
        time.sleep(0.005)
        continue
    b = os.read(0, min(room, 65536))
    if not b:
        break
    out.write(b)
    n += len(b)
)";

// pw-cat --record as a pair's capture runs it: 10 ms chunks at the rate it is asked, every sample
// 1000 for target "L", -2000 for "R"; target "gone" ends at once with an error, as a node that is
// not there does.
const char* kFakePwCat = R"(#!/usr/bin/env python3
import os, struct, sys, time
a = sys.argv
t = a[a.index('--target') + 1] if '--target' in a else ''
if t == 'gone':
    sys.stderr.write('error: target gone\n')
    sys.exit(1)
per = int(a[a.index('--rate') + 1]) // 100 * int(a[a.index('--channels') + 1])
chunk = struct.pack('<h', {'L': 1000, 'R': -2000}.get(t, 0)) * per
t0, n = time.monotonic(), 0
while True:
    if (time.monotonic() - t0) * 100 < n:
        time.sleep(0.002)
        continue
    os.write(1, chunk)
    n += 1
)";

std::string sink_state(const json& st, const std::string& target) {
  for (const json& k : st["sinks_status"])
    if (k["target"] == target) return k["state"].get<std::string>();
  return "";
}

long file_size(const std::string& p) {
  struct stat sb;
  return stat(p.c_str(), &sb) == 0 ? static_cast<long>(sb.st_size) : -1;
}

// Waits up to `ms` for `ok` on stream `id`.
template <typename F>
bool wait_for(AudioEngine& e, int id, int ms, F ok) {
  for (int i = 0; i < ms / 50; ++i) {
    if (ok(e.get(id))) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return ok(e.get(id));
}

void test_engine_sinks() {
  // As btbenchd (main.cpp): a sink process gone is a broken pipe to the pump, not a signal.
  std::signal(SIGPIPE, SIG_IGN);
  char tmpl[] = "/tmp/btbench-test-audio-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  mkdir((dir + "/bin").c_str(), 0755);
  {
    std::ofstream f(dir + "/bin/aplay");
    f << kFakeAplay;
  }
  // arecord: a capture that gives nothing, still running.
  {
    std::ofstream f(dir + "/bin/arecord");
    f << "#!/bin/sh\nexec sleep 60\n";
  }
  {
    std::ofstream f(dir + "/bin/pw-cat");
    f << kFakePwCat;
  }
  chmod((dir + "/bin/aplay").c_str(), 0755);
  chmod((dir + "/bin/arecord").c_str(), 0755);
  chmod((dir + "/bin/pw-cat").c_str(), 0755);
  const std::string path = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
  setenv("PATH", (dir + "/bin:" + path).c_str(), 1);
  {
    AudioEngine e([](const std::string&, const json&) {}, [](const std::string&) { return false; }, dir);
    json out;
    std::string err;
    int status = 0;
    const auto playing = [](const std::string& t) {
      return [t](const json& st) { return sink_state(st, t) == "playing"; };
    };

    // One stream into two sinks: both take it, at the rate it plays.
    CHECK(e.create(json::parse(R"({"source":{"type":"tone"},"sinks":[{"type":"alsa","target":"hw:A"},
        {"type":"alsa","target":"hw:B"}]})"), &out, &err, &status));
    const int one = out["id"];
    CHECK(wait_for(e, one, 3000, playing("hw:A")));
    CHECK(wait_for(e, one, 3000, playing("hw:B")));
    const long a0 = file_size(dir + "/hw:A"), b0 = file_size(dir + "/hw:B");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const long da = file_size(dir + "/hw:A") - a0, db = file_size(dir + "/hw:B") - b0;
    CHECK(da > 192000 / 2 && da < 192000 * 3 / 2);
    CHECK(db > 192000 / 2 && db < 192000 * 3 / 2);
    CHECK_EQ(e.get(one)["sink"]["target"].get<std::string>(), std::string("hw:A"));

    // A capture is not a local stream: it plays beside the local one.
    CHECK(e.create(json::parse(R"({"source":{"type":"capture","backend":"alsa","target":"hw:In"}})"), &out, &err, &status));
    const int cap = out["id"];

    // A second local stream takes the place of the first. One of its sinks takes nothing: it
    // stalls, and the other plays on at the stream's rate regardless.
    CHECK(e.create(json::parse(R"({"source":{"type":"noise"},"sinks":[{"type":"alsa","target":"hw:stall"},
        {"type":"alsa","target":"hw:A"}]})"), &out, &err, &status));
    const int two = out["id"];
    CHECK(e.get(one).is_null());
    CHECK_EQ(e.get(cap)["state"].get<std::string>(), std::string("running"));
    CHECK(wait_for(e, two, 3000, playing("hw:A")));
    CHECK(wait_for(e, two, 4000, [](const json& st) { return sink_state(st, "hw:stall") == "stalled"; }));
    const long a1 = file_size(dir + "/hw:A");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const long da1 = file_size(dir + "/hw:A") - a1;
    CHECK(da1 > 192000 / 2 && da1 < 192000 * 3 / 2);
    CHECK_EQ(sink_state(e.get(two), "hw:A"), std::string("playing"));

    // The sinks changed while it plays: the stalled one goes, B comes, A plays on as it was.
    CHECK(e.update(two, json::parse(R"({"sinks":[{"type":"alsa","target":"hw:A"},{"type":"alsa","target":"hw:B"}]})"),
                   &out, &err, &status));
    const int req = out["sink_req"];
    CHECK(wait_for(e, two, 3000, [req](const json& st) {
      return st["sink_seq"] == req && sink_state(st, "hw:B") == "playing" && sink_state(st, "hw:stall").empty();
    }));
    CHECK_EQ(sink_state(e.get(two), "hw:A"), std::string("playing"));
    CHECK_EQ(e.get(two)["sinks"].size(), size_t(2));

    // To listeners only: no sink left, the stream still running.
    CHECK(e.update(two, json::parse(R"({"sinks":[]})"), &out, &err, &status));
    CHECK(wait_for(e, two, 2000, [](const json& st) { return st["sinks_status"].empty() && st["sinks"].empty(); }));
    CHECK_EQ(e.get(two)["state"].get<std::string>(), std::string("running"));

    // A left/right pair: two mono captures, heard as one stereo stream, left from target and
    // right from target_right.
    CHECK(e.create(json::parse(R"({"source":{"type":"capture","target":"L","target_right":"R"}})"), &out, &err, &status));
    const int lr = out["id"];
    CHECK_EQ(out["source"]["target_right"].get<std::string>(), std::string("R"));
    AudioEngine::ListenerPtr l = e.listen(lr, false, &err, &status);
    CHECK(l != nullptr);
    std::string heard, piece;
    for (int i = 0; i < 20 && heard.size() < 44 + 48000 * 4 / 2; ++i)
      if (l && AudioEngine::take(*l, &piece, 200)) heard += piece;
    CHECK(heard.size() >= 44 + 4);
    if (heard.size() >= 44 + 4) {
      // The newest frame: by now the right capture runs too (the first frames may be its silence).
      int16_t f[2];
      std::memcpy(f, heard.data() + 44 + (heard.size() - 44) / 4 * 4 - 4, sizeof(f));
      CHECK_EQ(f[0], int16_t(1000));
      CHECK_EQ(f[1], int16_t(-2000));
    }
    if (l) e.unlisten(lr, l);
    CHECK_EQ(e.get(lr)["state"].get<std::string>(), std::string("running"));

    // Its right half gone ends it, as a capture whose source went away ends.
    CHECK(e.create(json::parse(R"({"source":{"type":"capture","target":"L","target_right":"gone"}})"), &out, &err, &status));
    const int half = out["id"];
    CHECK(wait_for(e, half, 3000, [](const json& st) { return st["state"] == "failed"; }));
    const std::string why = e.get(half)["error"];
    CHECK(why.find("right") != std::string::npos && why.find("target gone") != std::string::npos);

    e.stop_all();
  }
  setenv("PATH", path.c_str(), 1);
  std::filesystem::remove_all(dir);
}

// A coordinated set (CSIP earbuds): the leader, the combined node WirePlumber makes (without
// api.bluez5.set.members), and its two members' internal nodes; then the SPA plugin's own leader,
// which still names its members.
void test_endpoint_sets() {
  const json dump = json::parse(R"([
    {"id": 90, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink",
      "node.name": "bluez_output.set_8b1c2d", "device.api": "bluez5", "api.bluez5.address": "C0:00:00:00:00:01",
      "api.bluez5.set": "/org/bluez/hci0/set_8b1c2d", "api.bluez5.set.leader": true}}},
    {"id": 91, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink/Internal",
      "node.name": "bluez_output.C0_00_00_00_00_01.1", "device.api": "bluez5", "api.bluez5.address": "C0:00:00:00:00:01",
      "node.description": "Bud L", "api.bluez5.set": "/org/bluez/hci0/set_8b1c2d", "api.bluez5.internal": "true",
      "object.path": "/org/bluez/hci0/set_8b1c2d/C0:00:00:00:00:01-1"}}},
    {"id": 92, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink/Internal",
      "node.name": "bluez_output.C0_00_00_00_00_02.1", "device.api": "bluez5", "api.bluez5.address": "C0:00:00:00:00:02",
      "node.description": "Bud R", "api.bluez5.set": "/org/bluez/hci0/set_8b1c2d", "api.bluez5.internal": "true"}}},
    {"id": 93, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink/Internal",
      "node.name": "internal-other"}}}
  ])");
  const json s = endpoints_from_pw_dump(dump)["sinks"];
  CHECK_EQ(s.size(), size_t(3));  // an internal node of no set is not an endpoint
  const json* leader = nullptr;
  for (const json& e : s) {
    CHECK_EQ(e["set"].get<std::string>(), std::string("/org/bluez/hci0/set_8b1c2d"));
    if (e["set_leader"].get<bool>()) leader = &e;
    else CHECK(e["address"].is_string() && !e.contains("members"));
  }
  CHECK(leader);
  if (leader) {
    CHECK((*leader)["address"].is_null());
    CHECK_EQ((*leader)["members"], json::parse(R"(["C0:00:00:00:00:01", "C0:00:00:00:00:02"])"));
    CHECK_EQ((*leader)["label"].get<std::string>(), std::string("Bud L · set"));
    CHECK_EQ((*leader)["id"].get<std::string>(), std::string("bluez_output.set_8b1c2d"));
  }

  const json spa = json::parse(R"([
    {"id": 90, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink",
      "node.name": "bluez_output.set_8b1c2d", "node.description": "Buds", "api.bluez5.set": "/org/bluez/hci0/set_8b1c2d",
      "api.bluez5.set.leader": "true",
      "api.bluez5.set.members": "[{\"object.path\":\"/org/bluez/hci0/set_8b1c2d/C0:00:00:00:00:02-1\",\"channels\":[\"FR\"]},{\"object.path\":\"/org/bluez/hci0/set_8b1c2d/C0:00:00:00:00:01-1\",\"channels\":[\"FL\"]}]"}}},
    {"id": 94, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink",
      "node.name": "bluez_output.F8_DF_15_0A_11_3C.1", "device.api": "bluez5", "api.bluez5.address": "F8:DF:15:0A:11:3C"}}}
  ])");
  const json t = endpoints_from_pw_dump(spa)["sinks"];
  CHECK_EQ(t.size(), size_t(2));
  for (const json& e : t) {
    if (e["set_leader"].get<bool>()) {
      CHECK_EQ(e["members"], json::parse(R"(["C0:00:00:00:00:02", "C0:00:00:00:00:01"])"));
      CHECK_EQ(e["label"].get<std::string>(), std::string("Buds"));
    } else {
      CHECK(e["set"].is_null());
    }
  }
}


// The PipeWire view: every node with what the view shows of it, the links between nodes as one
// entry a pair, the defaults and the clock settings.
void test_graph() {
  const json dump = json::parse(R"J([
    {"id": 29, "type": "PipeWire:Interface:Node", "info": {"state": "suspended", "n-input-ports": 0, "n-output-ports": 0,
      "props": {"node.name": "Dummy-Driver", "node.driver": true, "priority.driver": 20000, "node.group": "pipewire.dummy"}}},
    {"id": 33, "type": "PipeWire:Interface:Node", "info": {"state": "running", "n-input-ports": 2, "n-output-ports": 2,
      "props": {"node.name": "alsa_output.card", "node.description": "Card", "media.class": "Audio/Sink",
                "node.driver": true, "object.serial": 33, "client.id": 32},
      "params": {"Format": [{"mediaType": "audio", "mediaSubtype": "raw", "format": "S16LE", "rate": 48000, "channels": 2, "position": ["FL", "FR"]}],
                 "Props": [{"volume": 1.0, "mute": true, "channelVolumes": [0.125, 0.125], "channelMap": ["FL", "FR"]}]}}},
    {"id": 44, "type": "PipeWire:Interface:Node", "info": {"state": "running", "n-input-ports": 0, "n-output-ports": 2,
      "props": {"node.name": "btbench-stream-1", "media.class": "Stream/Output/Audio", "node.latency": "1024/48000",
                "client.id": 43, "node.driver-id": 33, "node.autoconnect": "false"}}},
    {"id": 50, "type": "PipeWire:Interface:Node", "info": {"state": "idle",
      "props": {"node.name": "bluez_output.AA_BB_CC_DD_EE_FF.1", "media.class": "Audio/Sink/Internal", "device.api": "bluez5",
                "device.id": 60, "api.bluez5.profile": "a2dp-sink", "api.bluez5.codec": "sbc"}}},
    {"id": 60, "type": "PipeWire:Interface:Device", "info": {"props": {"device.name": "bluez_card.AA_BB_CC_DD_EE_FF",
      "device.api": "bluez5", "device.description": "Speaker", "api.bluez5.path": "/org/bluez/hci1/dev_AA_BB_CC_DD_EE_FF"}}},
    {"id": 43, "type": "PipeWire:Interface:Client", "info": {"props": {"application.name": "btbenchd",
      "application.process.binary": "pw-cat", "application.process.id": 1234}}},
    {"id": 45, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 44, "port.name": "output_FL", "audio.channel": "FL"}}},
    {"id": 46, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 44, "port.name": "output_FR", "audio.channel": "FR"}}},
    {"id": 39, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 33, "port.name": "playback_FL", "audio.channel": "FL"}}},
    {"id": 40, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 33, "port.name": "playback_FR", "audio.channel": "FR"}}},
    {"id": 70, "type": "PipeWire:Interface:Link", "info": {"output-node-id": 44, "output-port-id": 45, "input-node-id": 33,
      "input-port-id": 39, "state": "active", "error": null}},
    {"id": 71, "type": "PipeWire:Interface:Link", "info": {"output-port-id": 46, "input-port-id": 40, "state": "paused", "error": null}},
    {"id": 72, "type": "PipeWire:Interface:Link", "info": {"output-node-id": 44, "input-node-id": 99, "state": "active"}},
    {"id": 36, "type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "default"}, "metadata": [
      {"subject": 0, "key": "default.audio.sink", "type": "Spa:String:JSON", "value": {"name": "alsa_output.card"}},
      {"subject": 0, "key": "default.configured.audio.sink", "type": "Spa:String:JSON", "value": "{\"name\": \"bluez_output.gone\"}"}]},
    {"id": 31, "type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "settings"}, "metadata": [
      {"subject": 0, "key": "clock.rate", "value": 48000},
      {"subject": 0, "key": "clock.quantum", "value": "1024"},
      {"subject": 0, "key": "clock.allowed-rates", "value": "[ 44100, 48000 ]"},
      {"subject": 0, "key": "clock.force-quantum", "value": 0}]}
  ])J");
  const json g = graph_from_pw_dump(dump);
  const json& n = g["nodes"];
  CHECK_EQ(n.size(), size_t(4));
  // Sinks first (the card, then the Bluetooth one), streams, the driver last.
  CHECK_EQ(n[0]["id"].get<int>(), 33);
  CHECK_EQ(n[1]["id"].get<int>(), 50);
  CHECK_EQ(n[2]["kind"].get<std::string>(), std::string("stream_out"));
  CHECK_EQ(n[3]["kind"].get<std::string>(), std::string("driver"));
  // wpctl's scale: the cube root of what PipeWire keeps.
  CHECK_EQ(n[0]["volume"]["volume"].get<double>(), 0.5);
  CHECK(n[0]["volume"]["mute"].get<bool>());
  CHECK_EQ(n[0]["format"]["format"].get<std::string>(), std::string("S16LE"));
  CHECK_EQ(n[0]["format"]["rate"].get<int>(), 48000);
  CHECK(n[0]["default"].get<bool>());
  CHECK(!n[0]["configured"].get<bool>());
  CHECK(n[0]["can_default"].get<bool>());
  CHECK(!n[1]["can_default"].get<bool>());  // Internal: wpctl refuses it
  CHECK(n[1]["internal"].get<bool>());
  CHECK_EQ(n[1]["bluetooth"]["adapter"].get<std::string>(), std::string("hci1"));
  CHECK_EQ(n[1]["bluetooth"]["address"].get<std::string>(), std::string("AA:BB:CC:DD:EE:FF"));
  CHECK_EQ(n[1]["device"]["description"].get<std::string>(), std::string("Speaker"));
  CHECK(n[1]["volume"].is_null());
  CHECK(std::fabs(n[2]["latency_ms"].get<double>() - 21.333) < 0.001);
  CHECK_EQ(n[2]["driver_id"].get<int>(), 33);
  CHECK(!n[2]["driving"].get<bool>());
  CHECK(!n[2]["can_default"].get<bool>());
  CHECK_EQ(n[2]["autoconnect"], json(false));
  CHECK_EQ(n[2]["application"]["name"].get<std::string>(), std::string("btbenchd"));
  CHECK_EQ(n[2]["application"]["pid"].get<int>(), 1234);
  CHECK_EQ(n[3]["priority"]["driver"].get<int>(), 20000);
  CHECK(n[3]["format"].is_null());
  // Two port links of one pair (one found by its ports alone) are one entry; one to a node not in
  // the dump is dropped.
  CHECK_EQ(g["links"].size(), size_t(1));
  const json& l = g["links"][0];
  CHECK_EQ(l["output_node"].get<int>(), 44);
  CHECK_EQ(l["input_node"].get<int>(), 33);
  CHECK_EQ(l["ports"].size(), size_t(2));
  CHECK_EQ(l["ports"][1]["channel"].get<std::string>(), std::string("FR"));
  CHECK_EQ(l["ports"][1]["input"].get<std::string>(), std::string("playback_FR"));
  CHECK_EQ(l["state"].get<std::string>(), std::string("paused"));
  CHECK_EQ(g["defaults"]["sink"].get<std::string>(), std::string("alsa_output.card"));
  CHECK_EQ(g["defaults"]["configured_sink"].get<std::string>(), std::string("bluez_output.gone"));
  CHECK(g["defaults"]["source"].is_null());
  const json& c = g["clock"];
  CHECK_EQ(c["rate"].get<int>(), 48000);
  CHECK_EQ(c["quantum"].get<int>(), 1024);
  CHECK_EQ(c["allowed_rates"], json::parse("[44100, 48000]"));
  CHECK_EQ(c["force_quantum"].get<int>(), 0);
  CHECK(c["force_rate"].is_null());
  // No dump, and a dump without settings.
  const json e = graph_from_pw_dump(json::object());
  CHECK(e["nodes"].empty() && e["links"].empty() && e["clock"].is_null());
  CHECK(clock_from_pw_dump(json::array()).is_null());
}

std::string joined(const std::vector<std::string>& a) {
  std::string s;
  for (const std::string& x : a) s += (s.empty() ? "" : " ") + x;
  return s;
}

// What the PipeWire view's PUTs run.
void test_control_argv() {
  Tools t;
  t.wpctl = "/usr/bin/wpctl";
  t.pw_metadata = "/usr/bin/pw-metadata";
  std::vector<std::string> a;
  std::string err;
  CHECK(volume_argv(33, json{{"volume", 0.75}}, t, &a, &err));
  CHECK_EQ(joined(a), std::string("/usr/bin/wpctl set-volume 33 0.750"));
  CHECK(!volume_argv(33, json{{"volume", -0.1}}, t, &a, &err));
  CHECK(!volume_argv(33, json{{"volume", 1.6}}, t, &a, &err));
  CHECK(!volume_argv(33, json{{"volume", "50%"}}, t, &a, &err));
  CHECK(!volume_argv(33, json::object(), t, &a, &err));
  CHECK(mute_argv(33, json{{"mute", true}}, t, &a, &err));
  CHECK_EQ(a.back(), std::string("1"));
  CHECK(mute_argv(33, json{{"mute", "toggle"}}, t, &a, &err));
  CHECK_EQ(a.back(), std::string("toggle"));
  CHECK(!mute_argv(33, json{{"mute", "yes"}}, t, &a, &err));
  const json clock{{"rate", 48000}};
  CHECK(clock_argv(json{{"force_quantum", 256}}, clock, t, &a, &err));
  CHECK_EQ(joined(a), std::string("/usr/bin/pw-metadata -n settings 0 clock.force-quantum 256"));
  CHECK(clock_argv(json{{"force_quantum", nullptr}}, clock, t, &a, &err));
  CHECK_EQ(joined(a), std::string("/usr/bin/pw-metadata -n settings -d 0 clock.force-quantum"));
  CHECK(clock_argv(json{{"allowed_rates", {48000, 44100, 48000}}}, clock, t, &a, &err));
  CHECK_EQ(a.back(), std::string("[ 44100, 48000 ]"));
  // Default for the rates is the empty list, never a delete (which crashes PipeWire).
  CHECK(clock_argv(json{{"allowed_rates", nullptr}}, clock, t, &a, &err));
  CHECK_EQ(a.back(), std::string("[ ]"));
  CHECK(std::find(a.begin(), a.end(), "-d") == a.end());
  CHECK(!clock_argv(json{{"allowed_rates", {44100}}}, clock, t, &a, &err));  // without the clock's rate
  CHECK(clock_argv(json{{"allowed_rates", {44100}}}, json(nullptr), t, &a, &err));
  CHECK(!clock_argv(json{{"quantum", 3}}, clock, t, &a, &err));
  CHECK(!clock_argv(json{{"force_rate", 1000}}, clock, t, &a, &err));
  CHECK(!clock_argv(json{{"quantum", 256}, {"rate", 48000}}, clock, t, &a, &err));
  CHECK(!clock_argv(json{{"latency", 1}}, clock, t, &a, &err));
  t.pw_metadata.clear();
  CHECK(!clock_argv(json{{"quantum", 256}}, clock, t, &a, &err));
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
  test_pair_mix();
  test_endpoints();
  test_pw_dump_parse();
  test_play_order();
  test_cards();
  test_xml();
  test_upnp();
  test_engine_sinks();
  test_endpoint_sets();
  test_graph();
  test_control_argv();
  return report("test_audio");
}
