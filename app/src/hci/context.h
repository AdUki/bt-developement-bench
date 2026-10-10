#pragma once

// The context journal (docs/monitor.md "Context"): the packets of the live monitor stream that
// explain later ones, kept while they still apply, so each file of the capture ring can be given
// the context of the moment it began.
//
// The ring's files after the first start mid-session: the phone connected, its AVRCP browsing and
// A2DP media channels opened, its GATT database was discovered an hour and three files ago. On its
// own such a file says "CID 0x0076, 44 bytes" where it means AVRCP browsing, and btmon shows hex
// where it would name the characteristic. So the reader thread, which sees every packet anyway,
// keeps the context packets (ContextKind) of every live link and controller, drops a link's when
// it disconnects, and when btmon starts a new ring file writes them beside it as
// `<file>.ctx`: a btsnoop file (datalink 2001) the indexer and the btmon view read first.
//
// Bounded per link (signalling, AVDTP, ATT discovery each keep their newest), so a link that
// stays up for days costs at most a few hundred KiB.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

#include "packet_view.h"

namespace btb::hci {

class ContextJournal {
 public:
  static constexpr size_t kMaxSig = 400;     // per link
  static constexpr size_t kMaxAvdtp = 200;   // per link
  static constexpr size_t kMaxAtt = 1500;    // per link
  static constexpr size_t kMaxIndex = 16;    // per controller

  // Every packet of the live stream, in order.
  void packet(int64_t ts_us, uint16_t index, uint16_t mon, const uint8_t* d, size_t len);
  // Every packet of a btsnoop file, as if live: how a daemon that (re)starts mid-session learns
  // what is open, from the ring file being written and its own context. The number read.
  size_t replay(const std::string& path);

  // The context now, as a btsnoop file: header, then the records in the order they happened.
  std::string snapshot() const;
  size_t size() const;  // records held

  // Writes snapshot() to path (through a temporary file and a rename, so a reader never sees half
  // of one). false and *err on failure.
  bool write(const std::string& path, std::string* err) const;

 private:
  struct Rec {
    uint64_t seq;
    std::string bytes;  // a btsnoop record: header and packet
  };
  struct Link {
    std::deque<Rec> conn, sig, avdtp, att;
  };
  void keep(std::deque<Rec>* q, size_t cap, int64_t ts_us, uint16_t index, uint16_t mon,
            const uint8_t* d, size_t len);

  Classifier cls_;
  uint64_t seq_ = 0;
  std::map<uint16_t, std::deque<Rec>> index_;  // per controller
  std::map<uint32_t, Link> links_;             // index << 16 | handle
};

}  // namespace btb::hci
