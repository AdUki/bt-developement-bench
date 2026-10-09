#pragma once

// Byte-level helpers and the protocol constants the decoder needs. Defined here rather than taken
// from <bluetooth/hci.h>: libbluetooth's headers are on the board's SDK but not necessarily on a
// developer PC, and the handful of numbers below is all this module uses.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace btb::hci {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}
inline uint32_t le32(const uint8_t* p) {
  return le24(p) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline void put_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_be16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
inline void put_be32(uint8_t* p, uint32_t v) {
  put_be16(p, static_cast<uint16_t>(v >> 16));
  put_be16(p + 2, static_cast<uint16_t>(v));
}

// A BD_ADDR travels little-endian; humans read it most-significant byte first.
inline std::string bdaddr_str(const uint8_t* a) {
  char s[18];
  std::snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1],
                a[0]);
  return s;
}

// ---- Kernel socket API (include/net/bluetooth/hci_sock.h, hci_mon.h) ----
constexpr int kAfBluetooth = 31;
constexpr int kBtProtoHci = 1;
constexpr uint16_t kHciDevNone = 0xffff;
constexpr uint16_t kHciChannelMonitor = 2;
constexpr uint16_t kHciChannelLogging = 4;

struct SockaddrHci {
  unsigned short hci_family;
  unsigned short hci_dev;
  unsigned short hci_channel;
};

// Monitor channel opcodes: the same numbers btsnoop's datalink 2001 stores and pcap's
// LINUX_BT_MONITOR pseudo-header carries.
enum MonOpcode : uint16_t {
  kMonNewIndex = 0,
  kMonDelIndex = 1,
  kMonCommand = 2,
  kMonEvent = 3,
  kMonAclTx = 4,
  kMonAclRx = 5,
  kMonScoTx = 6,
  kMonScoRx = 7,
  kMonOpenIndex = 8,
  kMonCloseIndex = 9,
  kMonIndexInfo = 10,
  kMonVendorDiag = 11,
  kMonSystemNote = 12,
  kMonUserLogging = 13,
  kMonCtrlOpen = 14,
  kMonCtrlClose = 15,
  kMonCtrlCommand = 16,
  kMonCtrlEvent = 17,
  kMonIsoTx = 18,
  kMonIsoRx = 19,
};
constexpr size_t kMonHdrSize = 6;  // opcode, index, len — all le16

// ---- HCI ----
constexpr uint8_t kEvtConnComplete = 0x03;
constexpr uint8_t kEvtDisconnComplete = 0x05;
constexpr uint8_t kEvtCmdComplete = 0x0e;
constexpr uint8_t kEvtNumCompletedPackets = 0x13;
constexpr uint8_t kEvtSyncConnComplete = 0x2c;
constexpr uint8_t kEvtLeMeta = 0x3e;

constexpr uint8_t kLeConnComplete = 0x01;
constexpr uint8_t kLeEnhConnComplete = 0x0a;
constexpr uint8_t kLeCisEstablished = 0x19;
constexpr uint8_t kLeCisRequest = 0x1a;
constexpr uint8_t kLeBigComplete = 0x1b;
constexpr uint8_t kLeBigTerminate = 0x1c;
constexpr uint8_t kLeBigSyncEstablished = 0x1d;
constexpr uint8_t kLeBigSyncLost = 0x1e;
constexpr uint8_t kLeEnhConnCompleteV2 = 0x29;
constexpr uint8_t kLeCisEstablishedV2 = 0x2a;

constexpr uint16_t kCmdReadBufferSize = 0x1005;
constexpr uint16_t kCmdReadBdAddr = 0x1009;
constexpr uint16_t kCmdWriteSyncFlowControl = 0x0c2f;
constexpr uint16_t kCmdLeReadBufferSize = 0x2002;
constexpr uint16_t kCmdLeReadBufferSizeV2 = 0x2060;
constexpr uint16_t kCmdLeCreateCis = 0x2064;
constexpr uint16_t kCmdLeBigTerminateSync = 0x206c;

// ---- L2CAP ----
constexpr uint16_t kCidSignaling = 0x0001;
constexpr uint16_t kCidConnectionless = 0x0002;
constexpr uint16_t kCidAtt = 0x0004;
constexpr uint16_t kCidLeSignaling = 0x0005;
constexpr uint16_t kCidSmp = 0x0006;
constexpr uint16_t kCidSmpBredr = 0x0007;
constexpr uint16_t kCidDynamicStart = 0x0040;

constexpr uint8_t kSigConnReq = 0x02;
constexpr uint8_t kSigConnRsp = 0x03;
constexpr uint8_t kSigDisconnReq = 0x06;
constexpr uint8_t kSigDisconnRsp = 0x07;
constexpr uint8_t kSigLeConnReq = 0x14;
constexpr uint8_t kSigLeConnRsp = 0x15;
constexpr uint8_t kSigEcredConnReq = 0x17;
constexpr uint8_t kSigEcredConnRsp = 0x18;

constexpr uint16_t kPsmSdp = 0x0001;
constexpr uint16_t kPsmRfcomm = 0x0003;
constexpr uint16_t kPsmBnep = 0x000f;
constexpr uint16_t kPsmHidCtrl = 0x0011;
constexpr uint16_t kPsmHidIntr = 0x0013;
constexpr uint16_t kPsmAvctp = 0x0017;
constexpr uint16_t kPsmAvdtp = 0x0019;
constexpr uint16_t kPsmAvctpBrowsing = 0x001b;
constexpr uint16_t kPsmAtt = 0x001f;
constexpr uint16_t kPsmEatt = 0x0027;

}  // namespace btb::hci
