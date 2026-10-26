#pragma once

// What an ISO link carries (docs/monitor.md "ISO data"). An ISO packet says only its handle,
// sequence number and SDU length; which BIS or CIS that handle is, and how its SDUs are cut into
// codec frames, was said once when the stream was set up: a BIG created or synchronized to (and
// the BASE in its periodic advertising), or a CIG with its CISes and the ASEs that use them. The
// packet view's detail asks for it per ISO packet (iso_before() in packet_view.cpp): the setup
// packets before it, from the file and its context, are fed to an IsoTracker in order, and the
// tracker answers for the one handle.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace btb::hci {

// One ISO link as its setup described it. The codec fields are BAP's codec configuration
// (Assigned Numbers 6.12.5): an SDU is `blocks` blocks of one frame per channel, each `octets`
// long, channels in the order of their allocation bits (BAP 4.3; no allocation: one channel).
struct IsoStream {
  std::string link;          // "BIS 1 of BIG 0", "CIS 1 of CIG 0"; empty: no setup seen
  std::string config_from;   // where the codec configuration came from: "BASE, frame 4770"
  bool have_codec = false;
  uint8_t codec[5] = {};     // coding format, company, vendor codec ID
  uint8_t freq = 0;          // sampling frequency (Assigned Numbers 6.12.5.1); 0: not given
  uint8_t duration = 0xff;   // 0: 7.5 ms, 1: 10 ms; 0xff: not given
  bool have_alloc = false;
  uint32_t alloc = 0;        // audio channel allocation
  uint16_t octets = 0;       // octets per codec frame; 0: not given
  uint8_t blocks = 1;        // codec frame blocks per SDU
  std::string path;          // the data path set up for this direction: "HCI, transparent"

  unsigned channels() const;
  // The SDU the configuration describes, in bytes; 0 when it does not say enough.
  size_t sdu_bytes() const { return octets ? size_t{octets} * channels() * blocks : 0; }
};

// Fed the setup packets of a capture in order (commands, events and ATT notifications as the
// monitor channel carries them; anything else is ignored), it keeps per controller what each ISO
// handle is. n is the packet's frame number, for "config_from" (0: from the file's context).
class IsoTracker {
 public:
  void packet(uint16_t index, uint16_t mon, const uint8_t* d, size_t len, uint32_t n);
  // A BASE from a periodic advertising report: the search backwards for one feeds it last.
  void base_report(uint16_t index, const uint8_t* d, size_t len, uint32_t n);

  // What feeds the tracker, by the index entry alone. Setup: the few packets that set up BIGs,
  // CIGs, data paths and periodic advertising data, fed in order. A report: a periodic advertising
  // report, which may carry a BASE; there are many, so only the newest of the right sync is fed.
  static bool setup_packet(uint16_t mon, uint8_t evt, uint8_t sub, uint16_t opcode);
  static bool report_packet(uint16_t mon, uint8_t evt, uint8_t sub);

  // The ACL link (handle) a CIS belongs to, once its setup said; kNoAcl otherwise. Its ASE
  // notifications are what then still has to be fed.
  static constexpr uint16_t kNoAcl = 0xffff;
  uint16_t acl_of(uint16_t index, uint16_t handle) const;
  // The sync handle a receiver's BIS came from (its BASE is in that train's reports); 0xffff for
  // anything else.
  uint16_t sync_of(uint16_t index, uint16_t handle) const;

  // The stream on the handle, for a packet sent (tx) or received. sdu_len picks between two ASEs
  // on one CIS when the direction of neither is known.
  IsoStream stream(uint16_t index, uint16_t handle, bool tx, size_t sdu_len) const;

  // An ASE's value as its server notified it (ASCS 4.1) on the ACL link acl: uuid 0x2bc4 (sink),
  // 0x2bc5 (source) or 0 when discovery did not name it. local_server: this side is the server.
  void ase_state(uint16_t index, uint16_t acl, bool local_server, uint16_t uuid, const uint8_t* v, size_t n,
                 uint32_t frame);

 private:
  struct Config {
    bool have = false;
    uint8_t codec[5] = {};
    std::vector<uint8_t> ltvs;  // the codec configuration LTVs, level 2 then level 3
    std::string from;
  };
  struct Link {
    bool named = false;  // its BIG or CIG setup was seen, not only its data path or ACL
    bool bis = false;
    uint8_t group = 0;   // BIG handle or CIG ID
    uint8_t member = 0;  // BIS index or CIS ID
    uint16_t adv = 0xffff, sync = 0xffff;  // BIS: the advertising set or sync handle of its BIG
    uint16_t acl = kNoAcl;                 // CIS: its ACL link
    std::string path[2];                   // data path: [0] input (host → controller), [1] output
  };
  struct Big {
    uint16_t adv = 0xffff, sync = 0xffff;
    std::vector<uint8_t> bis;  // BIG Create Sync: the BIS indices asked for, in order
  };
  struct Ase {
    uint16_t uuid = 0;  // 0x2bc4 sink, 0x2bc5 source, 0 not known
    bool local_server = false;
    Config config;
    int cig = -1, cis = -1;
  };
  struct Base {
    std::vector<uint8_t> bytes;
    std::string from;
  };
  struct Controller {
    std::map<uint8_t, Big> bigs;
    std::map<uint8_t, std::vector<uint8_t>> cig_cis;   // Set CIG Parameters: CIS IDs, in order
    std::map<uint16_t, Link> links;                    // ISO handle
    std::map<uint16_t, Base> adv_base, sync_base;      // advertising set / sync handle
    std::map<uint16_t, std::vector<uint8_t>> adv_data; // fragments of periodic data being set
    std::map<uint32_t, Ase> ases;                      // server side << 24 | acl << 8 | ASE ID
  };

  static bool base_config(const std::vector<uint8_t>& base, uint8_t bis_index, Config* out);

  std::map<uint16_t, Controller> ctl_;
};

// The BASE (Basic Audio Announcement service data) among advertising data's AD structures; false
// when there is none.
bool find_base(const uint8_t* ad, size_t n, const uint8_t** base, size_t* base_len);

}  // namespace btb::hci
