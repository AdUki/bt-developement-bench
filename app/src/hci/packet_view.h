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

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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
};

constexpr uint16_t kNoHandle = 0xffff;
constexpr uint32_t kNoLat = 0xffffffff;
constexpr uint8_t kNoIndex = 0xff;

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

// Space-separated terms, all of which must match; "!term" negates one. Field terms answer from
// the index; any other word (or "quoted phrase") is a case-insensitive substring of the summary.
// The syntax is in docs/monitor.md.
struct Filter {
  struct Term {
    enum Kind {
      kType, kDir, kProto, kHandle, kCid, kPsm, kIndex, kOpcode, kEvt, kSub, kIs, kCmp, kText,
    } kind = kText;
    bool neg = false;
    std::vector<uint32_t> values;  // set membership ('|' in a value); bits for kType/kProto/kIs
    char field = 0;                // kCmp: 'l' len, 't' time, 'n' frame number, 'L' latency
    int op = 0;                    // kCmp: '<' '>' 'l' (<=) 'g' (>=) '='
    double num = 0;
    std::string text;  // kText, lower case
  };
  std::vector<Term> terms;
  bool has_text = false;
  // The time range the non-negated t terms allow, in seconds since the first packet (the graph
  // zooms to it).
  double t_lo = -1e300, t_hi = 1e300;
  std::string key;  // normalized source, for caching

  // false and *err on a malformed term (unknown is: value, bad number, empty value).
  static bool parse(const std::string& src, Filter* out, std::string* err);
  // The index-only part. t is seconds since the first packet, n the 1-based frame number.
  bool match_index(const IndexEntry& e, double t, uint32_t n) const;
  // The text part, given the summary in lower case.
  bool match_text(const std::string& lower_summary) const;
};

// ---- Decoding one packet ----

struct Dissection {
  std::string summary;
  // [{"name","value","off","len","children":[...]}] with off/len into the packet bytes.
  nlohmann::json fields;
};

// d/len: the HCI packet as the monitor channel carries it (no H4 type byte). The entry supplies
// what one packet cannot say about itself: direction, and the channel's protocol for L2CAP.
// With detail=false only the summary is built (the text filter calls this per packet).
void dissect(const IndexEntry& e, const uint8_t* d, size_t len, bool detail, Dissection* out);

// For the indexer: where the handle sits in a command (first parameter) — and whether the
// payload of an L2CAP channel is a failure (error response, reject, refused connection).
bool hci_cmd_has_handle(uint16_t opcode);
bool l2cap_payload_is_error(uint8_t proto, uint16_t cid, const uint8_t* d, size_t len);

// ---- One indexed file ----

class PacketIndex {
 public:
  PacketIndex();
  ~PacketIndex();
  PacketIndex(const PacketIndex&) = delete;
  PacketIndex& operator=(const PacketIndex&) = delete;

  // Everything below is called with mu held.
  std::mutex mu;

  bool open(const std::string& path, std::string* err);
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

  // A filter's matches over the indexed part, cached per filter string, and extended until the
  // deadline (text terms decode, so they may not finish in one request).
  struct Query {
    std::string key;
    Filter filter;
    std::vector<uint32_t> matches;  // entry indices
    size_t scanned = 0;             // entries looked at so far
  };
  Query* query(const Filter& f, int64_t deadline_us);

  // Per connection (controller index + handle; a reused handle accumulates), maintained while
  // indexing for the graph's connection list.
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
  const std::vector<ConnInfo>& conns() const { return conn_info_; }

  struct Tracker;

 private:
  bool fill(uint64_t off, size_t n);  // makes [off, off+n) available in buf_
  void reset();

  std::string path_;
  int fd_ = -1;
  uint64_t ino_ = 0, dev_ = 0;
  uint32_t datalink_ = 0;
  uint64_t file_size_ = 0;
  uint64_t next_off_ = 16;  // the next record to index
  bool at_end_ = false;
  std::vector<IndexEntry> entries_;
  std::vector<ConnInfo> conn_info_;
  std::unique_ptr<Tracker> tracker_;
  std::list<Query> queries_;  // most recently used first

  std::vector<uint8_t> buf_;
  uint64_t buf_off_ = 0;
  size_t buf_len_ = 0;
};

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
  // mutex for the duration of the request.
  struct ListParams {
    std::string filter;
    size_t start = 0;
    size_t count = 200;
    double at_t = -1;     // >= 0: start at the first match at or after this time (s)
    int64_t at_n = -1;    // >= 1: start at the match holding (or following) this frame
  };
  bool list(const std::string& name, const ListParams& p, nlohmann::json* out, int* status,
            std::string* err);
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

 private:
  std::string dir_;
  int budget_ms_ = 400;
  std::mutex mu_;
  std::list<std::pair<std::string, std::shared_ptr<PacketIndex>>> lru_;  // most recent first
};

}  // namespace btb::hci
