#pragma once

// btsnoop files (bluez src/shared/btsnoop.c): a 16-byte header — "btsnoop\0", version 1, datalink
// — then records of { orig len, incl len, flags, drops, timestamp } (all big-endian) + data. The
// timestamp is µs since 0000-01-01, offset 0x00E03AB44A676000 from the Unix epoch.
//
// Datalink 2001 (BlueZ monitor, what btmon -w writes) carries the controller index and monitor
// opcode in flags; 1002 (H4/UART, what Android and many vendor stacks write) and 1001 (raw HCI)
// are mapped to monitor opcodes for controller index 0, the way btsnoop_read_hci() does it.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace btb::hci {

constexpr uint32_t kBtsnoopHci = 1001;
constexpr uint32_t kBtsnoopUart = 1002;
constexpr uint32_t kBtsnoopMonitor = 2001;
constexpr int64_t kBtsnoopEpochDelta = 0x00E03AB44A676000LL;  // µs, year 0 → 1970

struct SnoopPacket {
  int64_t ts_us = 0;  // Unix epoch
  uint16_t index = 0;
  uint16_t opcode = 0;
  const uint8_t* data = nullptr;  // valid until the next read()
  size_t len = 0;
};

class BtsnoopReader {
 public:
  BtsnoopReader() = default;
  ~BtsnoopReader();
  BtsnoopReader(const BtsnoopReader&) = delete;
  BtsnoopReader& operator=(const BtsnoopReader&) = delete;

  bool open(const std::string& path, std::string* err);
  // false at end of file (or a truncated last record: btmon may still be writing it).
  bool read(SnoopPacket* out);
  uint32_t datalink() const { return datalink_; }

 private:
  FILE* f_ = nullptr;
  uint32_t datalink_ = 0;
  std::vector<uint8_t> buf_ = std::vector<uint8_t>(65536 + 16);
};

// Writes datalink 2001 files. Used by the tests to build captures; tiny and dependency-free.
class BtsnoopWriter {
 public:
  ~BtsnoopWriter();
  bool open(const std::string& path, uint32_t datalink = kBtsnoopMonitor);
  bool write(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* data, size_t len);
  // For datalink 1002: the H4 type byte is prepended, direction goes in flags bit 0.
  bool write_h4(int64_t ts_us, uint8_t h4_type, bool received, const uint8_t* data, size_t len);
  void close();

 private:
  bool record(int64_t ts_us, uint32_t flags, const uint8_t* pre, size_t pre_len,
              const uint8_t* data, size_t len);
  FILE* f_ = nullptr;
};

}  // namespace btb::hci
