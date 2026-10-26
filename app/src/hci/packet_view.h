#pragma once

// The packet view of the btsnoop captures (docs/monitor.md "Packet view"): a compact index of
// every packet in a file, a small filter language answered from that index, one-line summaries
// and a field-by-field decode of single packets, and time series for the graphs.
//
// Sized for the board: one ARMv6 core and 512 MB, ring files of up to 16 MiB (100k-300k packets),
// the newest of which btmon is still writing. So:
//   - a file is scanned once into 32 bytes per packet, never decoded again for structured filters;
//   - the file being written is indexed incrementally from where the last request stopped;
//   - at most kMaxIndexes files are kept indexed (about 10 MB for a full ring file);
//   - every request works against a deadline, and answers "not complete yet" with its progress
//     instead of holding one of the three HTTP workers for seconds.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "iso_stream.h"

namespace btb::hci {

// The kinds of packet the filter's type: term names. Derived from the monitor opcode.
enum PktType : uint8_t {
  kTypeCmd,
  kTypeEvt,
  kTypeAcl,
  kTypeSco,
  kTypeIso,
  kTypeIndex,  // new/delete/open/close index, index info
  kTypeLog,    // system notes and user logging (bluetoothd's log, btbench marks)
  kTypeMgmt,   // management channel traffic (ctrl open/close/command/event)
  kTypeDiag,   // vendor diagnostics
  kTypeOther,
};

// The topmost protocol the index knows a packet carries. For L2CAP that is decided by the channel:
// the fixed CIDs by number, dynamic channels by the PSM their Connection Request named.
enum Proto : uint8_t {
  kProtoNone,
  kProtoHci,  // commands and events
  kProtoL2cap,  // signalling channels, and channels whose protocol is unknown
  kProtoAtt,
  kProtoSmp,
  kProtoSdp,
  kProtoRfcomm,
  kProtoAvdtp,  // AVDTP signalling
  kProtoRtp,    // AVDTP media (transport) channel
  kProtoAvctp,
  kProtoBnep,
  kProtoHid,
  kProtoSco,
  kProtoIso,
  kProtoCount,
};

const char* pkt_type_name(uint8_t t);
const char* proto_name(uint8_t p);
uint8_t pkt_type_of(uint16_t mon_opcode);

enum EntryFlags : uint8_t {
  kFlagTx = 0x01,
  kFlagRx = 0x02,
  kFlagErr = 0x04,   // a failure status, a reject, an error response (filter is:err)
  kFlagCont = 0x08,  // an L2CAP continuation fragment: cid/psm/proto are its start fragment's
  kFlagMark = 0x10,  // a btbench mark (user logging with ident "btbench")
  kFlagVendor = 0x20,  // a vendor command (OGF 0x3f), its Command Complete/Status, a vendor event
  kFlagAdv = 0x40,     // an advertising or inquiry report, MGMT Device Found (filter is:adv)
};

constexpr uint16_t kNoHandle = 0xffff;
constexpr uint32_t kNoLat = 0xffffffff;
constexpr uint8_t kNoIndex = 0xff;
// A controller's company ID not known yet (what the kernel's Index Info says before setup reads it).
constexpr uint16_t kNoManufacturer = 0xffff;

// One packet, 32 bytes. Everything the structured filter terms and the graphs need, so neither
// touches the file.
struct IndexEntry {
  uint32_t off;     // file offset of the btsnoop record header
  uint32_t lat_us;  // TX data: HCI send → Number Of Completed Packets; kNoLat if none (yet)
  int64_t ts_us;    // Unix epoch
  uint16_t len;     // HCI packet length (the monitor payload), saturated at 65535
  uint16_t handle;  // ACL/SCO/ISO handle, or the handle a command/event is about; kNoHandle
  uint16_t cid;     // L2CAP CID as carried in the packet (ACL only), 0 = none
  uint16_t psm;     // PSM of that channel when the index saw it open, 0 = unknown/fixed
  uint16_t opcode;  // HCI command opcode: commands, and Command Complete/Status events
  uint8_t evt;      // HCI event code (events only)
  uint8_t sub;      // LE Meta subevent
  uint8_t index;    // controller index, kNoIndex for the system-wide notes
  uint8_t mon;      // monitor opcode (MonOpcode)
  uint8_t proto;    // Proto
  uint8_t flags;    // EntryFlags
};
static_assert(sizeof(IndexEntry) == 32, "the index is sized for the Zero W");

// ---- Filter ----

struct ConnInfo;

// Space-separated terms, all of which must match; "!term" negates one; "||" between groups of
// terms matches either group. Field terms answer from the index; any other word (or "quoted
// phrase") is a case-insensitive substring of the summary. The syntax is in docs/monitor.md.
struct Filter {
  struct Term {
    enum Kind {
      kType, kDir, kProto, kHandle, kCid, kPsm, kIndex, kOpcode, kEvt, kSub, kIs, kCmp, kText,
      kAddr,  // a peer: its links' packets (resolve()), and summaries that name it
    } kind = kText;
    bool neg = false;
    std::vector<uint32_t> values;  // set membership ('|' in a value); bits for kType/kProto/kIs
    char field = 0;                // kCmp: 'l' len, 't' time, 'n' frame number, 'L' latency
    int op = 0;                    // kCmp: '<' '>' 'l' (<=) 'g' (>=) '='
    double num = 0;
    std::string text;  // kText, kAddr: lower case
    std::vector<uint32_t> links;  // kAddr: index << 16 | handle of the links to that peer
  };
  std::vector<Term> terms;
  // "a || b": the groups, each a Filter of its own (terms is then empty).
  std::vector<Filter> alts;
  bool has_text = false;
  // The time range the non-negated t terms allow, in seconds since the first packet (the graph
  // zooms to it).
  double t_lo = -1e300, t_hi = 1e300;
  std::string key;  // normalized source, for caching

  // false and *err on a malformed term (unknown is: value, bad number, empty value).
  static bool parse(const std::string& src, Filter* out, std::string* err);
  // The index-only part: false if the packet cannot match, whatever its summary says. t is
  // seconds since the first packet, n the 1-based frame number.
  bool match_index(const IndexEntry& e, double t, uint32_t n) const;
  // The whole filter, given the summary in lower case (needed only when has_text), for a packet
  // that passed match_index.
  bool match_text(const IndexEntry& e, double t, uint32_t n, const std::string& lower_summary) const;
  // Points the addr: terms at the links to their peers, from the connections seen so far.
  void resolve(const std::vector<ConnInfo>& conns);
};

// ---- Decoding one packet ----

struct Dissection {
  std::string summary;
  // [{"name","value","off","len","children":[...]}] with off/len into the packet bytes.
  nlohmann::json fields;
};

// What ATT discovery said about one link's attributes, so a value can be decoded by the
// characteristic it belongs to (an ASE state, PAC records, a Broadcast Receive State) the way btmon's
// att.c does: the Characteristic declarations of Read By Type responses and the Find Information
// responses name the handles, and the last Read and Read By Type request each way say what a
// response answers. A link has two attribute servers, one per side: a server's PDUs (responses,
// notifications, the odd opcodes) and the client's requests to it are kept apart by direction.
// Built for the one packet the detail pane shows, from the ATT packets of its link before it.
struct GattDb {
  // One ATT PDU of the link (after the L2CAP header), in the order they were sent.
  void learn(const uint8_t* pdu, size_t n, bool tx);
  // The same from an ACL start fragment as the monitor carries it.
  void learn_acl(const uint8_t* d, size_t len, bool tx);
  // A new connection on the handle: its attributes are another device's.
  void clear();

  // The 16-bit UUID of an attribute (0: not seen, or a 128-bit one) of the server on the local
  // (true) or remote side.
  uint16_t uuid(bool local_server, uint16_t handle) const;
  // The characteristic a descriptor at handle belongs to: the nearest value handle below it.
  uint16_t owner(bool local_server, uint16_t handle) const;

  struct Side {
    std::vector<std::pair<uint16_t, uint16_t>> attrs;  // handle, UUID; sorted by handle
    std::vector<uint16_t> values;                      // characteristic value handles, sorted
    uint16_t read_handle = 0;  // the last Read (Blob) Request's handle
    uint16_t type_req = 0;     // the last Read By Type Request's type (16-bit)
  };
  Side side[2];  // [1]: the local server
};

// d/len: the HCI packet as the monitor channel carries it (no H4 type byte). The entry supplies
// what one packet cannot say about itself: direction, and the channel's protocol for L2CAP.
// With detail=false only the summary is built (the text filter calls this per packet). gatt, when
// given, decodes ATT values by their characteristic in the field tree (the summary stays the
// list's). manufacturer is the company of the packet's controller (Classifier::manufacturer()):
// vendor commands are decoded by it.
// iso, when given, is what the ISO packet's stream is: the detail names it and cuts the SDU into its
// codec frames.
void dissect(const IndexEntry& e, const uint8_t* d, size_t len, bool detail, Dissection* out,
             const GattDb* gatt = nullptr, uint16_t manufacturer = kNoManufacturer,
             const IsoStream* iso = nullptr);

// For the indexer: where the handle sits in a command (first parameter) — and whether the
// payload of an L2CAP channel is a failure (error response, reject, refused connection).
bool hci_cmd_has_handle(uint16_t opcode);
bool l2cap_payload_is_error(uint8_t proto, uint16_t cid, const uint8_t* d, size_t len);

// The JSON of one packet in a list (docs/monitor.md "Packet view"): n is its number in the view
// (the frame number of a file, the sequence number of the live stream), t its time in seconds
// since the view's first packet.
nlohmann::json packet_json(const IndexEntry& e, uint64_t n, double t, const std::string& summary);
// The same plus "fields" and "hex": the detail pane. The Frame node comes first; `frame_extra`
// nodes are put into it after the time (what this view numbers or measures from).
nlohmann::json packet_detail_json(const IndexEntry& e, uint64_t n, double t, const uint8_t* d,
                                  size_t len, const std::vector<nlohmann::json>& frame_extra,
                                  const GattDb* gatt = nullptr, uint16_t manufacturer = kNoManufacturer,
                                  const IsoStream* iso = nullptr);

// ---- Context: the packets that explain later ones ----

// What btmon and the indexer need to have seen to make sense of a packet that comes later: the
// controllers, the links' connection events (type and peer), L2CAP signalling (which PSM and mode
// a channel is), AVDTP signalling (which codec a stream carries) and ATT discovery (from which
// btmon's att.c names the characteristic a later value is about). A capture file holds what
// happened in it; the ring's files after the first start with everything already open, so each
// gets the context of the moment it began beside it (ContextJournal, `<file>.ctx`).
enum ContextKind : uint8_t {
  kCtxNone,
  kCtxIndex,  // New/Delete/Open/Close Index, Index Info
  kCtxLink,   // connection complete (BR/EDR, SCO, LE, CIS) and disconnection complete
  kCtxIso,    // what sets up ISO streams: BIGs, CIGs, data paths, periodic advertising data
  kCtxBase,   // a periodic advertising report: context only if it carries a BASE
  kCtxSig,    // L2CAP signalling, fragments included
  kCtxAvdtp,  // AVDTP signalling
  kCtxAtt,    // an ATT start fragment: context only if att_discovery() says so
};
ContextKind context_kind(const IndexEntry& e);
// Exchange MTU, Find Information, Read By Type and Read By Group Type, requests and responses,
// and Error Response. d/len: the ACL packet as the monitor carries it.
bool att_discovery(const uint8_t* d, size_t len);
// An ATT notification or indication whose value is an ASE state with a CIS (ASCS 4.1: QoS
// Configured to Disabling) or its codec configuration (Codec Configured). d/len: the ACL packet.
bool ase_notification(const uint8_t* d, size_t len);
// A periodic advertising report (LE Meta event) whose data holds a BASE.
bool base_report(const uint8_t* d, size_t len);

// ---- Classifying packets ----

// Per connection (controller index + handle; a reused handle accumulates), for the graph's
// connection list.
struct ConnInfo {
  uint16_t index = 0;
  uint16_t handle = 0;
  const char* type = "acl";
  std::string peer;
  int64_t first_us = 0, last_us = 0;
  uint64_t tx_bytes = 0, rx_bytes = 0;
  uint32_t tx_pkts = 0, rx_pkts = 0;
  uint32_t lat_n = 0;
};

// Turns monitor packets, in order, into IndexEntry values. It keeps the same bookkeeping as the
// live Decoder, cut down to what the index needs: links from their connection events (or first
// data), L2CAP channels from the signalling so a dynamic CID maps to its PSM and protocol,
// continuation fragments, and the HCI TX → NOCP FIFO. A capture file's index runs one over the
// file (its context first, then its records as they are written).
class Classifier {
 public:
  Classifier();
  ~Classifier();
  Classifier(const Classifier&) = delete;
  Classifier& operator=(const Classifier&) = delete;

  // Fills *e from one packet: everything but off, and lat_us stays kNoLat. id is the caller's
  // name for this packet: when a later NOCP completes it, on_latency(id, µs) is called, so the
  // caller can update wherever it keeps the entry.
  void classify(int64_t ts_us, uint16_t index, uint16_t mon, const uint8_t* d, size_t len,
                uint32_t id, IndexEntry* e);
  std::function<void(uint32_t id, uint32_t lat_us)> on_latency;

  // Zeroes the per-connection counts, keeping the links and channels: after classifying a file's
  // context, which happened before the file and is not its traffic.
  void reset_counts();

  const std::vector<ConnInfo>& conns() const;

  // The company ID a controller index reported (Index Info, Read Local Version Information), or
  // kNoManufacturer. The latest one is kept, not one per packet: btintel downloads the firmware
  // before the kernel's Index Info names the controller, and those commands are Intel's too.
  uint16_t manufacturer(uint8_t index) const;

  struct State;

 private:
  std::unique_ptr<State> st_;
};

// ---- One indexed file ----

class PacketIndex {
 public:
  PacketIndex();
  ~PacketIndex();
  PacketIndex(const PacketIndex&) = delete;
  PacketIndex& operator=(const PacketIndex&) = delete;

  // Everything below is called with mu held.
  std::mutex mu;

  // Reads path + ".ctx" first, when there is one (ContextJournal): the links and channels open
  // when the file began.
  bool open(const std::string& path, std::string* err);
  // That context file's records (a btsnoop file without its header); empty if there is none.
  const std::string& context_records() const { return ctx_records_; }
  // Re-checks the file: false if it is gone or was replaced/truncated (the caller then drops
  // this index and builds a new one). A file that grew is simply indexed further next time.
  bool still_valid() const;
  // Indexes more of the file until it reaches the current end (true) or the deadline passes.
  bool extend(int64_t deadline_us);
  bool at_end() const { return at_end_; }

  size_t size() const { return entries_.size(); }
  const IndexEntry& at(size_t i) const { return entries_[i]; }
  int64_t t0_us() const { return entries_.empty() ? 0 : entries_[0].ts_us; }
  uint64_t indexed_bytes() const { return next_off_; }
  uint64_t file_size() const { return file_size_; }
  uint32_t datalink() const { return datalink_; }
  const std::string& path() const { return path_; }

  // The HCI packet bytes of entry i, valid until the next read(). nullptr if unreadable.
  const uint8_t* read(size_t i, size_t* len);

  // The summary of entry i (decodes it).
  std::string summary(size_t i);

  // Entry i as a btsnoop record of datalink 2001 (whatever this file's datalink), appended to
  // *out. false if the packet cannot be read.
  bool append_record(size_t i, std::string* out);

  // A filter's matches over the indexed part, cached per filter string, and extended until the
  // deadline (text terms decode, so they may not finish in one request).
  struct Query {
    std::string key;
    Filter filter;
    std::vector<uint32_t> matches;  // entry indices
    size_t scanned = 0;             // entries looked at so far
  };
  Query* query(const Filter& f, int64_t deadline_us);

  // Maintained while indexing, for the graph's connection list.
  const std::vector<ConnInfo>& conns() const { return cls_.conns(); }
  uint16_t manufacturer(uint8_t index) const { return cls_.manufacturer(index); }

 private:
  bool fill(uint64_t off, size_t n);  // makes [off, off+n) available in buf_
  void load_context(const std::string& path);

  std::string path_;
  std::string ctx_records_;
  int fd_ = -1;
  uint64_t ino_ = 0, dev_ = 0;
  uint32_t datalink_ = 0;
  uint64_t file_size_ = 0;
  uint64_t next_off_ = 16;  // the next record to index
  bool at_end_ = false;
  std::vector<IndexEntry> entries_;
  Classifier cls_;
  std::list<Query> queries_;  // most recently used first

  std::vector<uint8_t> buf_;
  uint64_t buf_off_ = 0;
  size_t buf_len_ = 0;
};

// What ATT discovery on the link of entry i said before it (dissect()'s gatt): the file's context,
// then the link's ATT packets from the file's start; a connection event on the handle starts over.
void gatt_before(PacketIndex& idx, size_t i, GattDb* g);
// What the ISO stream of entry i is (dissect()'s iso): the file's context, then the setup packets
// of the file before it (iso_stream.h).
void iso_before(PacketIndex& idx, size_t i, IsoStream* s);

// ---- The cache of indexed files, shared by the HTTP workers ----

class PacketStore {
 public:
  static constexpr size_t kMaxIndexes = 2;

  explicit PacketStore(std::string dir) : dir_(std::move(dir)) {}

  // The index of dir/name, built (or extended) lazily by the caller under its mutex. Null and
  // *status (404/400/415/500) + *err if the file cannot be used.
  std::shared_ptr<PacketIndex> get(const std::string& name, int* status, std::string* err);

  // How long one request may index and filter (tests make it tiny to exercise partial answers).
  void set_budget_ms(int ms) { budget_ms_ = ms; }
  int budget_ms() const { return budget_ms_; }

  // The route bodies (docs/monitor.md "Packet view" has the shapes). Each takes the index's
  // mutex while it indexes, filters and builds its page, never while a tail request waits.
  //
  // A tail request (wait_ms > 0) is how the console and `bench mon packets` follow the file
  // being written (docs/monitor.md "The live end"): when the page from `start` would be empty
  // and the index and filter are complete, list() waits for the file to grow until a new match
  // is there, the file goes, a new ring file appears in the directory ("newer" in the answer:
  // after a rotation the live end is in that file), or wait_ms pass (an empty page). A page that
  // is not full is answered kTailBatchMs after its first match was in hand, so a burst comes as
  // one page rather than one request per packet. The file is watched with inotify for exactly
  // the time the request waits, so nothing is watched while nobody looks. Each waiting request
  // holds an HTTP worker: at most kMaxWaiters wait at once (503 beyond).
  static constexpr int kMaxWaitMs = 30000;
  static constexpr int kTailBatchMs = 150;
  static constexpr int kMaxWaiters = 4;
  // How often a waiting tail request asks whether its client is still there. The console aborts
  // its tail on every filter change, and httplib does not hand out the socket to wait on with the
  // file; without the check each change would keep a place taken for the whole wait.
  static constexpr int kGoneCheckMs = 500;
  struct ListParams {
    std::string filter;
    size_t start = 0;
    size_t count = 200;
    double at_t = -1;     // >= 0: start at the first match at or after this time (s)
    int64_t at_n = -1;    // >= 1: start at the match holding (or following) this frame
    int wait_ms = 0;      // > 0: a tail request, waiting up to this long (capped at kMaxWaitMs)
    // Whether the client has gone (it aborted the request): a tail request then stops waiting
    // and frees its place, within kGoneCheckMs.
    std::function<bool()> client_gone;
  };
  bool list(const std::string& name, const ListParams& p, nlohmann::json* out, int* status,
            std::string* err);
  int waiters() const { return waiters_.load(std::memory_order_relaxed); }
  bool packet(const std::string& name, uint32_t n, nlohmann::json* out, int* status,
              std::string* err);
  static constexpr size_t kMaxPoints = 2000;
  struct GraphParams {
    std::string filter;
    int64_t bucket_ms = 0;  // 0: pick one that gives at most `points` buckets
    size_t points = kMaxPoints;  // the client's width in buckets (a canvas has no use for more)
    double from = -1, to = -1;  // seconds since the first packet; -1: the filter's t range or all
    int conn_index = -1, conn_handle = -1;  // -1: the busiest connection
  };
  bool graph(const std::string& name, const GraphParams& p, nlohmann::json* out, int* status,
             std::string* err);

  // Statistics of what a filter matches (docs/monitor.md "Statistics"): packets and bytes per
  // protocol, per packet kind and per link, errors, marks, TX latency; from the index alone.
  bool stats(const std::string& name, const std::string& filter, nlohmann::json* out, int* status,
             std::string* err);

  // The packets a filter matches, as a btsnoop file of datalink 2001 in *out (btmon -r and
  // Wireshark read it). 409 while the filter is not worked out to the end of the file yet: the
  // client lists until "complete" first. *count is the number of matches in it. With context,
  // the packets btmon needs to decode them (the file's .ctx, then the context packets of the
  // file among the matches) are in it too, in order.
  bool export_filtered(const std::string& name, const std::string& filter, bool context,
                       std::string* out, size_t* count, int* status, std::string* err);


 private:
  std::string dir_;
  int budget_ms_ = 400;
  std::mutex mu_;
  std::list<std::pair<std::string, std::shared_ptr<PacketIndex>>> lru_;  // most recent first
  std::atomic<int> waiters_{0};  // tail requests waiting right now (32-bit: ARMv6)
};

}  // namespace btb::hci
