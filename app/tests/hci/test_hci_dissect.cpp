// The packet view's dissector on single packets: summaries and field trees of AVDTP (codecs,
// content protection, errors), AVRCP (control and browsing), SDP records, RFCOMM AT commands and
// OBEX. Most byte strings are real packets from the boards' captures (a Samsung phone, and two
// boards streaming PipeWire's Opus); the rest are built per spec where no capture had one.

#include <string>

#include "../check.h"
#include "packet_view.h"
#include "packets.h"
#include "wire.h"

using namespace hcitest;
using nlohmann::json;
using btb::hci::Dissection;
using btb::hci::IndexEntry;

namespace {

Bytes from_hex(const std::string& h) {
  Bytes b;
  for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
  return b;
}

Dissection run(const Bytes& pkt, uint8_t proto, uint16_t psm, bool rx = true) {
  IndexEntry e{};
  e.mon = rx ? btb::hci::kMonAclRx : btb::hci::kMonAclTx;
  e.proto = proto;
  e.psm = psm;
  Dissection ds;
  btb::hci::dissect(e, pkt.data(), pkt.size(), true, &ds);
  // The summary alone (the text filter's path) must be the same.
  Dissection brief;
  btb::hci::dissect(e, pkt.data(), pkt.size(), false, &brief);
  CHECK_EQ(brief.summary, ds.summary);
  return ds;
}

Dissection run_hex(const std::string& hex, uint8_t proto, uint16_t psm) { return run(from_hex(hex), proto, psm); }

// The value of the first field called name, anywhere in the tree; "(none)" if there is none.
std::string field(const json& nodes, const std::string& name) {
  for (const json& n : nodes) {
    if (n.value("name", std::string()) == name) return n.value("value", std::string());
    if (n.contains("children")) {
      const std::string v = field(n["children"], name);
      if (v != "(none)") return v;
    }
  }
  return "(none)";
}

bool has(const std::string& s, const std::string& needle) {
  if (s.find(needle) != std::string::npos) return true;
  std::printf("  '%s' not in '%s'\n", needle.c_str(), s.c_str());
  return false;
}

constexpr uint8_t kAvdtp = btb::hci::kProtoAvdtp;
constexpr uint8_t kAvctp = btb::hci::kProtoAvctp;

// An AVRCP vendor-dependent frame on the control channel (AVCTP + AV/C + PDU header).
Bytes avrcp(uint8_t label, bool rsp, uint8_t ctype, uint8_t pdu, const Bytes& params, uint8_t pt = 0) {
  Bytes b{static_cast<uint8_t>(label << 4 | (rsp ? 0x02 : 0x00)), 0x11, 0x0e, ctype, 0x48, 0x00, 0x00, 0x19, 0x58, pdu, pt};
  add16be(b, static_cast<uint16_t>(params.size()));
  return acl(11, 2, l2cap(0x0044, cat(b, params)));
}

Bytes attr(uint32_t id, const std::string& v) {
  Bytes b;
  add32be(b, id);
  add16be(b, 0x006a);
  add16be(b, static_cast<uint16_t>(v.size()));
  for (char c : v) b.push_back(static_cast<uint8_t>(c));
  return b;
}

void test_avdtp() {
  // PipeWire's Opus endpoint capabilities, then a configuration of it (two boards).
  auto d = run_hex("0b20260022004200720c0100071a00fff105000005108000ffffff0f1f00000000000000000000000800", kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("GET_ALL_CAPABILITIES accept: Opus (PipeWire) 128 ch, 2.5/5/10/20/40 ms, any bitrate, "
                                  "delay reporting"));
  CHECK_EQ(field(d.fields, "Vendor ID"), std::string("0x000005f1 (The Linux Foundation)"));
  CHECK_EQ(field(d.fields, "Audio location"), std::string("0x0fffffff (any)"));
  d = run_hex("0b00280024004200800348440100071a00fff105000005100201030000000438010000000000000000000800", kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("SET_CONFIGURATION cmd acp 18 int 17: Opus (PipeWire) 2 ch, 10 ms, 319 kbit/s, delay reporting"));
  CHECK_EQ(field(d.fields, "Maximum bitrate"), std::string("319488 bit/s"));
  CHECK_EQ(field(d.fields, "Coupled streams"), std::string("1"));
  CHECK_EQ(field(d.fields, "Audio location"), std::string("0x00000003 (front left, front right)"));
  // Google's Opus (the phone's choice towards the board), and LDAC capabilities.
  d = run_hex("0b20170013004300000304100100070900ffe00000000100920800", kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("SET_CONFIGURATION cmd acp 1 int 4: Opus (Google) 48000 Hz, stereo, 20 ms, delay reporting"));
  CHECK_EQ(field(d.fields, "Frame duration"), std::string("20 ms"));
  d = run_hex("0b20160012004200520c0100070a00ff2d010000aa003c070800", kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("GET_ALL_CAPABILITIES accept: LDAC 44100/48000/88200/96000 Hz, mono/dual channel/stereo, "
                                  "delay reporting"));
  CHECK_EQ(field(d.fields, "Vendor ID"), std::string("0x0000012d (Sony)"));
  // A reject names its error.
  d = run_hex("0b20070003004200b30831", kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("CLOSE reject: BAD_STATE (0x31)"));
  // SBC with SCMS-T content protection: every field, and the bitrate at the maximum bitpool.
  Bytes caps{0x04, 0x0c, 0x01, 0x00};
  caps = cat(caps, sbc_cap());
  caps = cat(caps, Bytes{0x04, 0x02, 0x02, 0x00});
  d = run(acl(11, 0, l2cap(0x0042, avdtp(3, 0, 0x03, caps))), kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("SET_CONFIGURATION cmd acp 1 int 3: SBC 48000 Hz, joint stereo, 16 blocks, 8 subbands, "
                                  "loudness, bitpool 2..53, SCMS-T"));
  CHECK_EQ(field(d.fields, "Bitrate at the maximum bitpool"), std::string("357000 bit/s"));
  CHECK_EQ(field(d.fields, "Block length"), std::string("16"));
  CHECK_EQ(field(d.fields, "Content Protection"), std::string("0x0002 (SCMS-T)"));
  // A SET_CONFIGURATION reject: the failing category and an A2DP error.
  d = run(acl(11, 2, l2cap(0x0042, avdtp(3, 3, 0x03, Bytes{0x07, 0xc4}))), kAvdtp, 0x19);
  CHECK_EQ(d.summary, std::string("SET_CONFIGURATION reject: NOT_SUPPORTED_SAMPLING_FREQUENCY (0xc4) (Media Codec)"));
  // AAC (A2DP 1.4 object types).
  d = run(acl(11, 2, l2cap(0x0042, avdtp(3, 2, 0x0c, Bytes{0x07, 0x08, 0x00, 0x02, 0xc2, 0xff, 0xfc, 0x84, 0xe2, 0x00}))), kAvdtp, 0x19);
  CHECK(has(d.summary, "AAC MPEG-2 AAC LC/MPEG-4 AAC LC/MPEG-4 AAC-ELDv2, 8000/"));
  CHECK(has(d.summary, "96000 Hz, 1 ch/2 ch, 320 kbps VBR"));
}

void test_avrcp_control() {
  // From the captures: notifications, play status, capabilities, a key press.
  auto d = run_hex("0b201a001600450022110e0d480000195831000009020000000000000001", kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification CHANGED TRACK_CHANGED: track 0x0000000000000001"));
  d = run_hex("0b2013000f00440072110e0f4800001958310000020d7f", kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification INTERIM VOLUME_CHANGED: 127 (100%)"));
  d = run_hex("0b2017001300440032110e0f480000195831000006080203010201", kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification INTERIM PLAYER_APPLICATION_SETTING_CHANGED: Shuffle off, Repeat off"));
  CHECK_EQ(field(d.fields, "Repeat"), std::string("off"));
  d = run_hex("0b2013000f00450012110e0d4800001958310000020101", kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification CHANGED PLAYBACK_STATUS_CHANGED: playing"));
  d = run_hex("0b201a001600450012110e0c480000195830000009ffffffff0000006a01", kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("GetPlayStatus STABLE: playing 0:00.106 / n/a"));
  CHECK_EQ(field(d.fields, "Song position"), std::string("106 ms (0:00.106)"));
  d = run_hex("0b201c001800440002110e0c48000019581000000b03090102030408090a0b0d", kAvctp, 0x17);
  CHECK(has(d.summary, "GetCapabilities STABLE: events PLAYBACK_STATUS_CHANGED TRACK_CHANGED TRACK_REACHED_END"));
  CHECK(has(d.summary, "ADDRESSED_PLAYER_CHANGED VOLUME_CHANGED"));
  d = run_hex("0b000c0008004500b0110e00487c4400", kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("PASS THROUGH PLAY pressed CONTROL"));

  // Built per spec: volume changes, absolute volume, a rejected registration.
  d = run(avrcp(3, true, 0x0d, 0x31, Bytes{0x0d, 0x57}), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification CHANGED VOLUME_CHANGED: 87 (69%)"));
  d = run(avrcp(4, false, 0x00, 0x50, Bytes{0x40}), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("SetAbsoluteVolume CONTROL: 64 (50%)"));
  d = run(avrcp(5, true, 0x0a, 0x31, Bytes{0x01}), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification REJECTED: Invalid Parameter (0x01)"));
  d = run(avrcp(6, true, 0x0d, 0x31, Bytes{0x05, 0x00, 0x01, 0x23, 0x45}), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("RegisterNotification CHANGED PLAYBACK_POS_CHANGED: 1:14.565"));
  d = run(avrcp(7, false, 0x00, 0x14, Bytes{0x02, 0x02, 0x03, 0x03, 0x02}), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("SetPlayerApplicationSettingValue CONTROL: Repeat all tracks, Shuffle all tracks"));

  // GetElementAttributes: the request, then the response with the track's metadata.
  Bytes req{0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0x01, 0, 0, 0, 0x07};
  d = run(avrcp(8, false, 0x01, 0x20, req), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("GetElementAttributes STATUS: Title, Playing time"));
  Bytes rsp{7};
  rsp = cat(rsp, attr(1, "Over the Horizon"));
  rsp = cat(rsp, attr(2, "Samsung"));
  rsp = cat(rsp, attr(3, "Galaxy"));
  rsp = cat(rsp, attr(4, "3"));
  rsp = cat(rsp, attr(5, "12"));
  rsp = cat(rsp, attr(6, "Pop"));
  rsp = cat(rsp, attr(7, "205000"));
  d = run(avrcp(8, true, 0x0c, 0x20, rsp), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("GetElementAttributes STABLE: Title \"Over the Horizon\", Artist \"Samsung\", "
                                  "Album \"Galaxy\", Track 3, Tracks 12, Genre \"Pop\", Time 3:25"));
  CHECK_EQ(field(d.fields, "Playing time"), std::string("205000 ms (3:25.000)"));
  CHECK_EQ(field(d.fields, "Album"), std::string("\"Galaxy\""));
  // Too long for one AV/C frame: the start decodes as far as it goes, the continuation is named.
  Bytes part(rsp.begin(), rsp.begin() + 30);
  d = run(avrcp(8, true, 0x0c, 0x20, part, 1), kAvctp, 0x17);
  CHECK(has(d.summary, "GetElementAttributes STABLE (start): Title \"Over the Horizon\""));
  d = run(avrcp(8, true, 0x0c, 0x20, Bytes(10, 0x41), 2), kAvctp, 0x17);
  CHECK_EQ(d.summary, std::string("GetElementAttributes STABLE (continue, 10 bytes)"));
}

void test_avrcp_browsing() {
  // The browsing channel (ERTM): a phone's folders, a board's now-playing list and an item.
  auto d = run_hex(
      "0c20a500a1004600422212110e710097040000000602001700000000000000010500006a00094661766f726974657302001700000000"
      "000000020500006a0009506c61796c6973747302001400000000000000030100006a0006547261636b7302001400000000000000040200"
      "006a0006416c62756d7302001500000000000000050300006a00074172746973747302001500000000000000060000006a0007466f6c64"
      "6572733e42",
      kAvctp, 0x1b);
  CHECK_EQ(d.summary, std::string("GetFolderItems rsp: 6 items: folder \"Favorites\", folder \"Playlists\", folder \"Tracks\", "
                                  "folder \"Albums\", …"));
  CHECK_EQ(field(d.fields, "Folder type"), std::string("0x05 (playlists)"));
  d = run_hex(
      "0b20c100bd004600160c72110e7100b30400010002030058000000000000000800006a000b5768697465206e6f6973650400000001006a"
      "000b5768697465206e6f69736500000002006a0007627462656e636800000003006a000c54657374207369676e616c7300000004006a00"
      "0138030050000000000000000900006a000753696c656e63650400000001006a000753696c656e636500000002006a0007627462656e63"
      "6800000003006a000c54657374207369676e616c7300000004006a0001398f9b",
      kAvctp, 0x1b);
  CHECK_EQ(d.summary, std::string("GetFolderItems rsp: 2 items: \"White noise\", \"Silence\""));
  CHECK_EQ(field(d.fields, "Artist"), std::string("\"btbench\""));
  d = run_hex(
      "0b2061005d004600100942110e730053040500000001006a000a53696e652031206b487a00000002006a0007627462656e636800000003"
      "006a000c54657374207369676e616c7300000004006a00013100000006006a000b54657374207369676e616c3614",
      kAvctp, 0x1b);
  CHECK_EQ(d.summary, std::string("GetItemAttributes rsp: Title \"Sine 1 kHz\", Artist \"btbench\", Album \"Test signals\", "
                                  "Track 1, Genre \"Test signal\""));
  d = run_hex("0b20180014004500140b62110e70000a04000100000002006a00e2c0", kAvctp, 0x1b);
  CHECK_EQ(d.summary, std::string("SetBrowsedPlayer rsp: 2 items in /"));
}

void test_sdp_rfcomm_obex() {
  auto d = run_hex(
      "0b0071006d00400007000600680065356335610900000a0001000f090001350619111f191203090004350c35031901003505190003080d"
      "09000535031910020900093508350619111e090109090100251848616e64732d4672656520566f6963652067617465776179090301080109"
      "031109012000",
      btb::hci::kProtoSdp, 0x01);
  CHECK_EQ(d.summary, std::string("Service Search Attribute Response (tid 6) 101 bytes: Handsfree Audio Gateway "
                                  "\"Hands-Free Voice gateway\" channel 13 Handsfree 1.9"));
  CHECK_EQ(field(d.fields, "Service Class ID List (0x0001)"), std::string("Handsfree Audio Gateway, Generic Audio"));
  CHECK_EQ(field(d.fields, "Protocol Descriptor List (0x0004)"), std::string("(L2CAP), (RFCOMM 0x0d)"));
  d = run_hex("0b2013000f0042003bef170d0a2b4243533a20330d0aa5", btb::hci::kProtoRfcomm, 0x03);
  CHECK_EQ(d.summary, std::string("UIH dlci 14: +BCS: 3 (LC3-SWB)"));
  // BIP cover art over an L2CAP channel in ERTM (GOEP 2.0): Connect, its response, a Get.
  d = run_hex("0c0022001e007500000080001a10007fff4600137163dd544a7e11e2b47c0050c249004805a6", btb::hci::kProtoL2cap, 0x1001);
  CHECK_EQ(d.summary, std::string("OBEX Connect max 32767, Target BIP Cover Art"));
  d = run_hex("0c202700230040000001a0001f10000400cb000000014a00137163dd544a7e11e2b47c0050c249004805c2", btb::hci::kProtoL2cap,
              0x1001);
  CHECK_EQ(d.summary, std::string("OBEX Success (0xa0) max 1024, Who BIP Cover Art"));
  CHECK_EQ(field(d.fields, "Connection ID"), std::string("0x00000001"));
  d = run_hex("0c00350031007500020183002dcb000000019701420010782d62742f696d672d74686d0030001300300030003000300030003000310000df62",
              btb::hci::kProtoL2cap, 0x1001);
  CHECK_EQ(d.summary, std::string("OBEX Get final, SRM, Type \"x-bt/img-thm\""));
  CHECK_EQ(field(d.fields, "Img-Handle"), std::string("\"0000001\""));
}

// One monitor packet of any kind (a command, an event, a management frame, an ACL packet), with
// the summary-only path checked against the detail one as run() does.
Dissection run_mon(uint16_t mon, const std::string& hex, uint8_t proto = 0, uint16_t cid = 0) {
  const Bytes pkt = from_hex(hex);
  IndexEntry e{};
  e.mon = static_cast<uint8_t>(mon);
  e.proto = proto;
  e.cid = cid;
  e.flags = mon == btb::hci::kMonAclTx ? btb::hci::kFlagTx : btb::hci::kFlagRx;
  Dissection ds;
  btb::hci::dissect(e, pkt.data(), pkt.size(), true, &ds);
  Dissection brief;
  btb::hci::dissect(e, pkt.data(), pkt.size(), false, &brief);
  CHECK_EQ(brief.summary, ds.summary);
  return ds;
}

Dissection cmd(const std::string& hex) { return run_mon(btb::hci::kMonCommand, hex); }
Dissection evt(const std::string& hex) { return run_mon(btb::hci::kMonEvent, hex); }

// A decoded layout leaves no raw parameter block behind.
bool no_blob(const json& fields) {
  for (const char* n : {"Parameters", "Return parameters", "Undecoded"}) {
    if (field(fields, n) != "(none)") {
      std::printf("  '%s' left: %s\n", n, field(fields, n).c_str());
      return false;
    }
  }
  return true;
}

void test_hci_advertising() {
  // The parameters btmon shows for LE Set Extended Advertising Parameters, which the dissector
  // used to leave as one blob (bluetoothd's legacy connectable advertising).
  auto d = cmd("362019011300a00000f00000070000000000000000007f0100010000");
  CHECK_EQ(d.summary, std::string("LE Set Extended Advertising Parameters set 1 100.00-150.00 ms legacy"));
  CHECK_EQ(field(d.fields, "Properties"), std::string("0x0013 (connectable, scannable, legacy)"));
  CHECK_EQ(field(d.fields, "Min interval"), std::string("100.00 ms (0xa0)"));
  CHECK_EQ(field(d.fields, "Channel map"), std::string("0x07 (37, 38, 39)"));
  CHECK_EQ(field(d.fields, "TX power"), std::string("no preference (0x7f)"));
  CHECK_EQ(field(d.fields, "Secondary PHY"), std::string("0x01 (LE 1M)"));
  CHECK_EQ(field(d.fields, "Scan request notifications"), std::string("disabled (0x00)"));
  CHECK(no_blob(d.fields));
  d = evt("0e050136200008");
  CHECK_EQ(d.summary, std::string("LE Set Extended Advertising Parameters complete TX power 8 dBm"));
  CHECK_EQ(field(d.fields, "Selected TX power"), std::string("8 dBm"));
  d = cmd("392006010101000000");
  CHECK_EQ(d.summary, std::string("LE Set Extended Advertising Enable on set 1"));
  CHECK_EQ(field(d.fields, "Set 0"), std::string("handle 1, duration none, max events 0"));
  // An Auracast source (the VIM3L): the Broadcast ID, then the BASE in the periodic advertising.
  d = cmd("37200b0103010706165218cb6dae");
  CHECK_EQ(d.summary, std::string("LE Set Extended Advertising Data set 1 Broadcast ID 0xae6dcb"));
  CHECK_EQ(field(d.fields, "Operation"), std::string("0x03 (complete data)"));
  d = cmd("3f202901032625165118409c0001010600000000100201080202010503030000000304640004030204000100");
  CHECK_EQ(d.summary, std::string("LE Set Periodic Advertising Data set 1 BASE 1 BIS, LC3 48 kHz, 10 ms, FL+FR, 100 octets, media"));
  CHECK_EQ(field(d.fields, "Presentation delay"), std::string("40000 us"));
  CHECK_EQ(field(d.fields, "Audio channel allocation"), std::string("0x00000003 (FL, FR)"));
  CHECK_EQ(field(d.fields, "Streaming audio contexts"), std::string("0x0004 (media)"));
  CHECK_EQ(field(d.fields, "BIS index"), std::string("1"));
  CHECK(no_blob(d.fields));
  d = cmd("3e200701100010000000");
  CHECK_EQ(d.summary, std::string("LE Set Periodic Advertising Parameters set 1 20.00-20.00 ms"));
  d = cmd("4020020101");
  CHECK_EQ(d.summary, std::string("LE Set Periodic Advertising Enable on set 1"));
  // A Realtek earbud's extended advertising: the LE Audio announcements of CAP, ASCS and TMAS.
  d = evt("3e5b0d0101000020002d1517840102007fba000000000000000000410201040f0952544c383736334520417564696f041653180109164e18"
          "01270223020003164f18051655182a0009024e184f18501844180319420906ff5d00020100");
  CHECK_EQ(d.summary, std::string("LE Extended Advertising Report 84:17:15:2D:00:20 (public) -70 dBm \"RTL8763E Audio\" "
                                  "CAP targeted, ASCS targeted"));
  CHECK_EQ(field(d.fields, "Event type"), std::string("0x0001 (connectable; data complete)"));
  CHECK_EQ(field(d.fields, "Secondary PHY"), std::string("0x02 (LE 2M)"));
  CHECK_EQ(field(d.fields, "TX power"), std::string("not available"));
  CHECK_EQ(field(d.fields, "Flags"), std::string("0x04 (BR/EDR not supported)"));
  CHECK_EQ(field(d.fields, "Available sink contexts"), std::string("0x0227 (unspecified, conversational, media, voice assistants, ringtone)"));
  CHECK_EQ(field(d.fields, "TMAP role"), std::string("0x002a (call terminal, unicast media receiver, broadcast media receiver)"));
  CHECK_EQ(field(d.fields, "Appearance"), std::string("0x0942 (wearable audio device)"));
  CHECK_EQ(field(d.fields, "Manufacturer data"), std::string("company 0x005d (Realtek), 020100"));
  CHECK(no_blob(d.fields));
  // Scanning: two PHYs, each with its own parameters.
  d = cmd("41200d00010500600018000020014800");
  CHECK_EQ(d.summary, std::string("LE Set Extended Scan Parameters passive"));
  CHECK_EQ(field(d.fields, "Scanning PHYs"), std::string("0x05 (LE 1M, LE Coded)"));
  CHECK_EQ(field(d.fields, "Filter policy"), std::string("0x01 (accept list only)"));
  CHECK(no_blob(d.fields));
  // The rest built per spec (no capture had them): periodic sync, its report with a BASE, BIGInfo.
  d = cmd("44200e0000016655443322c10000e80300");
  CHECK_EQ(d.summary, std::string("LE Periodic Advertising Create Sync C1:22:33:44:55:66 SID 0"));
  CHECK_EQ(field(d.fields, "Sync timeout"), std::string("10000.00 ms (0x3e8)"));
  d = evt("3e100e00010000016655443322c1021000" "05");
  CHECK_EQ(d.summary, std::string("LE Periodic Advertising Sync Established sync handle 1 C1:22:33:44:55:66 SID 0 interval 20.00 ms"));
  CHECK_EQ(field(d.fields, "Clock accuracy"), std::string("0x05 (50 ppm)"));
  d = evt("3e2e0f01007fc4ff002625165118409c0001010600000000100201080202010503030000000304640004030204000100");
  CHECK_EQ(d.summary, std::string("LE Periodic Advertising Report sync handle 1 -60 dBm, 38 bytes BASE 1 BIS, LC3 48 kHz, 10 ms, "
                                  "FL+FR, 100 octets, media"));
  CHECK_EQ(field(d.fields, "Octets per codec frame"), std::string("100"));
  d = evt("3e1422010002040800020002640010270064000200" "01");
  CHECK_EQ(d.summary, std::string("LE BIGInfo Advertising Report sync handle 1, 2 BIS, ISO interval 10.00 ms, max SDU 100, encrypted"));
  CHECK(no_blob(d.fields));
}

void test_hci_iso() {
  // A unicast stream's group (the VIM3L towards the Realtek earbud): two CIS, then their handles.
  auto d = cmd("622021004c1d004c1d0000000008000800020078003c00020202020100003c0002020002");
  CHECK_EQ(d.summary, std::string("LE Set CIG Parameters CIG 0, 2 CIS"));
  CHECK_EQ(field(d.fields, "SDU interval C to P"), std::string("7500 us"));
  CHECK_EQ(field(d.fields, "Max SDU C to P"), std::string("120"));
  CHECK_EQ(field(d.fields, "PHY C to P"), std::string("0x02 (LE 2M)"));
  CHECK(no_blob(d.fields));
  d = evt("0e0a01622000000200090109");
  CHECK_EQ(d.summary, std::string("LE Set CIG Parameters complete CIG 0 CIS 2304,2305"));
  // Its CIS once established, built per spec.
  d = evt("3e1d19000009401f00401f00102700102700020203010101017800" "3c000600");
  CHECK_EQ(d.summary, std::string("LE CIS Established handle 2304 ISO interval 7.50 ms, latency 10.0/10.0 ms"));
  CHECK_EQ(field(d.fields, "CIG sync delay"), std::string("8000 us"));
  CHECK_EQ(field(d.fields, "Max PDU P to C"), std::string("60"));
  CHECK(no_blob(d.fields));
  // A broadcast (the VIM3L's Auracast source): the BIG, its completion, the data path, the end.
  d = cmd("68201f000101102700c8001400040200000000000000000000000000000000000000");
  CHECK_EQ(d.summary, std::string("LE Create BIG BIG 0 adv set 1, 1 BIS"));
  CHECK_EQ(field(d.fields, "Max transport latency"), std::string("20 ms"));
  CHECK_EQ(field(d.fields, "RTN"), std::string("4"));
  CHECK(no_blob(d.fields));
  d = evt("3e151b0000d81f00e846000208020004c800100001000a");
  CHECK_EQ(d.summary, std::string("LE Create BIG Complete BIG 0 BIS 2560, ISO interval 20.00 ms, latency 18.2 ms"));
  CHECK_EQ(field(d.fields, "BIG sync delay"), std::string("8152 us"));
  CHECK_EQ(field(d.fields, "IRC"), std::string("4"));
  d = cmd("6e200d000a0000030000000000000000");
  CHECK_EQ(d.summary, std::string("LE Setup ISO Data Path handle 2560 input HCI transparent"));
  d = evt("0e06016e2000000a");
  CHECK_EQ(d.summary, std::string("LE Setup ISO Data Path complete handle 2560"));
  d = evt("3e031c0016");
  CHECK_EQ(d.summary, std::string("LE Terminate BIG Complete BIG 0 reason 0x16 (Connection Terminated By Local Host)"));
  // A receiver's side, built per spec: sync to two BIS, and the controller's answer.
  d = cmd("6b201a00010000" + std::string(32, '0') + "00e803020102");
  CHECK_EQ(d.summary, std::string("LE BIG Create Sync BIG 0 sync handle 1 BIS 1,2"));
  d = evt("3e131d000010270004020002640008000200" "0a010a");
  CHECK_EQ(d.summary, std::string("LE BIG Sync Established BIG 0 BIS 2560,2561, ISO interval 10.00 ms, latency 10.0 ms"));
  // An ISO data packet: the boundary and the packet status in the tree.
  d = run_mon(btb::hci::kMonIsoRx, "002a0c000100c8800102030405060708");
  CHECK_EQ(d.summary, std::string("ISO seq 1, SDU 200 bytes (lost)"));
  CHECK_EQ(field(d.fields, "Packet boundary"), std::string("2 (complete SDU)"));
  CHECK_EQ(field(d.fields, "Packet status"), std::string("lost"));
}

void test_hci_controller() {
  // The Intel BE200's answers at init: versions, features, codecs.
  auto d = evt("0e0c010110000d23470d02002347");
  CHECK_EQ(field(d.fields, "HCI version"), std::string("13 (Bluetooth 5.4, revision 0x4723)"));
  CHECK_EQ(field(d.fields, "Manufacturer"), std::string("2 (Intel)"));
  CHECK(no_blob(d.fields));
  d = evt("0e0c01032000fff901ffbe000000");
  CHECK(has(field(d.fields, "Features"), "CIS central, CIS peripheral, isochronous broadcaster, synchronized receiver"));
  d = evt("0e23010d100007000201020202030e04020502060c03020000000102000200010600060004");
  CHECK_EQ(field(d.fields, "Supported codecs"), std::string("7"));
  CHECK_EQ(field(d.fields, "Vendor codec"), std::string("0x0002:0x0000 (Intel), 0x01 (BR/EDR ACL)"));
  CHECK(no_blob(d.fields));
  d = evt("0e0c01031000bffe0ffedbff7b87");
  CHECK(has(field(d.fields, "Features"), "EDR ACL 2 Mb/s, EDR ACL 3 Mb/s"));
  d = cmd("0e100706000000000200");
  CHECK_EQ(d.summary, std::string("Read Local Supported Codec Capabilities LC3 (0x06)"));
  CHECK_EQ(field(d.fields, "Transport"), std::string("0x02 (BR/EDR SCO)"));
  // A connection's start: the enhanced LE connection, with the fields that used to stay bytes.
  d = evt("3e1f0a000008000020002d15178400000000000000000000000018000000f40100");
  CHECK_EQ(d.summary, std::string("LE Enhanced Connection Complete handle 2048 84:17:15:2D:00:20 (public) central interval 30.00 ms"));
  CHECK_EQ(field(d.fields, "Central clock accuracy"), std::string("500 ppm"));
  CHECK(no_blob(d.fields));
  // Classic: the HFP eSCO set-up's codec, built per spec (mSBC over a transparent air mode).
  d = cmd("3d043b0100401f0000401f0000030000000003000000003c003c00007d0000007d0000"
          "050000000005000000001000100001010f0f0101101008000803" "02");
  CHECK_EQ(d.summary, std::string("Enhanced Setup Synchronous Connection handle 1 transparent"));
  CHECK_EQ(field(d.fields, "Input coding format"), std::string("mSBC (0x05)"));
  CHECK_EQ(field(d.fields, "Retransmission effort"), std::string("0x02 (optimize for link quality)"));
  CHECK(no_blob(d.fields));
  // L2CAP's information responses: the bits by name.
  d = run_mon(btb::hci::kMonAclRx, "0b0010000c0001000b01080002000000b8020000", btb::hci::kProtoL2cap, 1);
  CHECK_EQ(field(d.fields, "Features"), std::string("0x000002b8 (enhanced retransmission, streaming, FCS option, fixed "
                                                    "channels, unicast connectionless data)"));
  d = run_mon(btb::hci::kMonAclRx, "0b001400100001000b020c00030000008600000000000000", btb::hci::kProtoL2cap, 1);
  CHECK_EQ(field(d.fields, "Fixed channels"), std::string("8600000000000000 (L2CAP signalling, connectionless, BR/EDR "
                                                          "security manager)"));
}

}  // namespace

int main() {
  test_avdtp();
  test_avrcp_control();
  test_avrcp_browsing();
  test_sdp_rfcomm_obex();
  test_hci_advertising();
  test_hci_iso();
  test_hci_controller();
  return report("test_hci_dissect");
}
