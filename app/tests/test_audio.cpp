// The audio streams' decisions: the generators' levels and continuity, the meter, the WAV
// framing, what a stream body may say and the command lines it becomes, the endpoint lists made
// from pw-dump and from BlueZ's transports, and the UPnP parsers. The pumping itself (processes,
// pipes) is exercised with `make run` against the desktop's PipeWire and a radio stream.
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

  CHECK(endpoint_id_ok("bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp"));
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
    {"id": 44, "type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Video/Source", "node.name": "v4l2"}}},
    {"id": 0, "type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "default"}, "metadata": [
      {"subject": 0, "key": "default.configured.audio.sink", "value": {"name": "gone"}},
      {"subject": 0, "key": "default.audio.sink", "value": {"name": "bluez_output.F8_DF_15_0A_11_3C.1"}},
      {"subject": 0, "key": "default.audio.source", "value": "{\"name\": \"bluez_input.5C_E9_1E_22_40_01.2\"}"}]}
  ])");
  const json e = endpoints_from_pw_dump(dump);
  CHECK_EQ(e["sinks"].size(), size_t(2));
  CHECK_EQ(e["sources"].size(), size_t(1));
  const json& jbl = e["sinks"][0];  // Bluetooth first
  CHECK_EQ(jbl["kind"].get<std::string>(), std::string("bluetooth"));
  CHECK_EQ(jbl["address"].get<std::string>(), std::string("F8:DF:15:0A:11:3C"));
  CHECK(jbl["default"].get<bool>());
  CHECK_EQ(jbl["serial"].get<std::string>(), std::string("120"));
  CHECK_EQ(e["sinks"][1]["kind"].get<std::string>(), std::string("loopback"));
  CHECK(!e["sinks"][1]["default"].get<bool>());
  const json& pix = e["sources"][0];
  CHECK_EQ(pix["address"].get<std::string>(), std::string("5C:E9:1E:22:40:01"));  // from the node name
  CHECK(pix["default"].get<bool>());  // the metadata value as a string, too
  CHECK(endpoints_from_pw_dump(json::object())["sinks"].empty());

  const json media = json::parse(R"({"transports": [
    {"address": "F8:DF:15:0A:11:3C", "uuid": "110a", "state": "idle", "codec": {"name": "SBC", "rate": 44100}},
    {"address": "5C:E9:1E:22:40:01", "uuid": "110b", "state": "active", "codec": {"name": "AAC", "rate": 48000}},
    {"address": "00:1A:7D:DA:71:13", "uuid": "111f", "codec": {"name": "mSBC"}},
    {"address": "11:22:33:44:55:66", "uuid": "184e"}]})");
  const json a = endpoints_from_alsa(media, true);
  CHECK_EQ(a["sinks"].size(), size_t(3));    // A2DP source, HFP, loopback
  CHECK_EQ(a["sources"].size(), size_t(3));  // A2DP sink, HFP, loopback
  CHECK_EQ(a["sinks"][0]["id"].get<std::string>(), std::string("bluealsa:DEV=F8:DF:15:0A:11:3C,PROFILE=a2dp"));
  CHECK_EQ(a["sinks"][0]["rate"].get<int>(), 44100);
  CHECK_EQ(a["sources"][0]["id"].get<std::string>(), std::string("bluealsa:DEV=5C:E9:1E:22:40:01,PROFILE=a2dp"));
  CHECK_EQ(a["sinks"][1]["profile"].get<std::string>(), std::string("sco"));
  CHECK_EQ(a["sinks"][2]["id"].get<std::string>(), std::string("hw:Loopback,0,0"));
  for (const json& x : a["sinks"]) CHECK(endpoint_id_ok(x["id"].get<std::string>()));
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
  test_xml();
  test_upnp();
  return report("test_audio");
}
