#include "btsnoop.h"

#include <cerrno>
#include <cstring>

#include "wire.h"

namespace btb::hci {

namespace {

const uint8_t kMagic[8] = {'b', 't', 's', 'n', 'o', 'o', 'p', 0};

// btsnoop_read_hci()'s mapping for files that are not in monitor format.
uint16_t opcode_from_flags(uint8_t type, uint32_t flags) {
  const bool rx = flags & 0x01;
  switch (type) {
    case 0x01: return kMonCommand;
    case 0x02: return rx ? kMonAclRx : kMonAclTx;
    case 0x03: return rx ? kMonScoRx : kMonScoTx;
    case 0x04: return kMonEvent;
    case 0x05: return rx ? kMonIsoRx : kMonIsoTx;
    case 0xff:
      if (flags & 0x02) return rx ? kMonEvent : kMonCommand;
      return rx ? kMonAclRx : kMonAclTx;
    default: return 0xffff;
  }
}

}  // namespace

BtsnoopReader::~BtsnoopReader() {
  if (f_) std::fclose(f_);
}

bool BtsnoopReader::open(const std::string& path, std::string* err) {
  f_ = std::fopen(path.c_str(), "rbe");
  if (!f_) {
    if (err) *err = path + ": " + std::strerror(errno);
    return false;
  }
  uint8_t hdr[16];
  if (std::fread(hdr, 1, sizeof(hdr), f_) != sizeof(hdr) || std::memcmp(hdr, kMagic, 8) != 0) {
    if (err) *err = path + ": not a btsnoop file";
    return false;
  }
  datalink_ = be32(hdr + 12);
  if (datalink_ != kBtsnoopMonitor && datalink_ != kBtsnoopUart && datalink_ != kBtsnoopHci) {
    if (err) *err = path + ": unsupported btsnoop datalink " + std::to_string(datalink_);
    return false;
  }
  return true;
}

bool BtsnoopReader::read(SnoopPacket* out) {
  if (!f_) return false;
  for (;;) {
    uint8_t rec[24];
    if (std::fread(rec, 1, sizeof(rec), f_) != sizeof(rec)) return false;
    const uint32_t incl = be32(rec + 4);
    const uint32_t flags = be32(rec + 8);
    const uint64_t raw_ts = (static_cast<uint64_t>(be32(rec + 16)) << 32) | be32(rec + 20);
    if (incl > buf_.size()) return false;  // corrupt: nothing after it can be trusted
    if (std::fread(buf_.data(), 1, incl, f_) != incl) return false;

    out->ts_us = static_cast<int64_t>(raw_ts) - kBtsnoopEpochDelta;
    out->data = buf_.data();
    out->len = incl;
    switch (datalink_) {
      case kBtsnoopMonitor:
        out->index = static_cast<uint16_t>(flags >> 16);
        out->opcode = static_cast<uint16_t>(flags & 0xffff);
        return true;
      case kBtsnoopUart:
        if (incl < 1) continue;
        out->index = 0;
        out->opcode = opcode_from_flags(buf_[0], flags);
        out->data = buf_.data() + 1;
        out->len = incl - 1;
        if (out->opcode == 0xffff) continue;
        return true;
      default:
        out->index = 0;
        out->opcode = opcode_from_flags(0xff, flags);
        return true;
    }
  }
}

BtsnoopWriter::~BtsnoopWriter() { close(); }

bool BtsnoopWriter::open(const std::string& path, uint32_t datalink) {
  close();
  f_ = std::fopen(path.c_str(), "wbe");
  if (!f_) return false;
  uint8_t hdr[16];
  std::memcpy(hdr, kMagic, 8);
  put_be32(hdr + 8, 1);
  put_be32(hdr + 12, datalink);
  return std::fwrite(hdr, 1, sizeof(hdr), f_) == sizeof(hdr);
}

bool BtsnoopWriter::record(int64_t ts_us, uint32_t flags, const uint8_t* pre, size_t pre_len,
                           const uint8_t* data, size_t len) {
  if (!f_) return false;
  uint8_t rec[24];
  const uint32_t total = static_cast<uint32_t>(pre_len + len);
  const uint64_t ts = static_cast<uint64_t>(ts_us + kBtsnoopEpochDelta);
  put_be32(rec, total);
  put_be32(rec + 4, total);
  put_be32(rec + 8, flags);
  put_be32(rec + 12, 0);
  put_be32(rec + 16, static_cast<uint32_t>(ts >> 32));
  put_be32(rec + 20, static_cast<uint32_t>(ts));
  if (std::fwrite(rec, 1, sizeof(rec), f_) != sizeof(rec)) return false;
  if (pre_len && std::fwrite(pre, 1, pre_len, f_) != pre_len) return false;
  return !len || std::fwrite(data, 1, len, f_) == len;
}

bool BtsnoopWriter::write(int64_t ts_us, uint16_t index, uint16_t opcode, const uint8_t* data,
                          size_t len) {
  return record(ts_us, (static_cast<uint32_t>(index) << 16) | opcode, nullptr, 0, data, len);
}

bool BtsnoopWriter::write_h4(int64_t ts_us, uint8_t h4_type, bool received, const uint8_t* data,
                             size_t len) {
  uint32_t flags = received ? 0x01 : 0x00;
  if (h4_type == 0x01 || h4_type == 0x04) flags |= 0x02;
  return record(ts_us, flags, &h4_type, 1, data, len);
}

void BtsnoopWriter::close() {
  if (f_) std::fclose(f_);
  f_ = nullptr;
}

}  // namespace btb::hci
