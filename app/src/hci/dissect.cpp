// One packet, decoded: the one-line summary of the packet list (Wireshark's Info column) and the
// field tree of the detail pane. Modelled on bluez monitor/packet.c, l2cap.c, att.c, sdp.c,
// rfcomm.c, avdtp.c and avctp.c, but much shallower: names and the parameters people look for,
// not every bit.
//
// The summary is what the text filter searches, so it is built for every packet a text term
// visits. The field tree is only built for the detail view: every field goes through the F()/OPEN()
// macros, whose arguments (the string formatting) are not evaluated in summary mode.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "codec.h"
#include "mgmt_names.h"
#include "packet_view.h"
#include "wire.h"

namespace btb::hci {

using nlohmann::json;

namespace {

std::string sf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string sf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return {};
  if (static_cast<size_t>(n) < sizeof(buf)) return std::string(buf, static_cast<size_t>(n));
  std::string s(static_cast<size_t>(n) + 1, '\0');
  va_start(ap, fmt);
  std::vsnprintf(&s[0], s.size(), fmt, ap);
  va_end(ap);
  s.resize(static_cast<size_t>(n));
  return s;
}

std::string hexbytes(const uint8_t* p, size_t n, size_t max = 32) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  const size_t m = n < max ? n : max;
  s.reserve(m * 2 + 3);
  for (size_t i = 0; i < m; ++i) {
    s.push_back(digits[p[i] >> 4]);
    s.push_back(digits[p[i] & 15]);
  }
  if (n > max) s += "…";
  return s;
}

// Text a device or a stack sent (a name, an AT command, a log line), made safe for one line.
std::string printable(const uint8_t* p, size_t n, size_t max = 80) {
  std::string s;
  for (size_t i = 0; i < n && s.size() < max; ++i) {
    const uint8_t c = p[i];
    if (c == 0) break;
    if (c == '\r' || c == '\n') {
      if (!s.empty() && s.back() != ' ') s.push_back(' ');
    } else if (c >= 0x20 && c != 0x7f) {
      s.push_back(static_cast<char>(c));  // UTF-8 bytes pass through
    } else {
      s.push_back('.');
    }
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// Text a device sent (a title, a folder name, a service name), quoted, cut for the summary.
std::string quoted(const uint8_t* p, size_t n, size_t max = 60) {
  return "\"" + printable(p, n, max) + (n > max ? "…" : "") + "\"";
}

bool mostly_text(const uint8_t* p, size_t n) {
  if (n == 0) return false;
  size_t ok = 0;
  for (size_t i = 0; i < n; ++i) {
    if ((p[i] >= 0x20 && p[i] < 0x7f) || p[i] == '\r' || p[i] == '\n') ++ok;
  }
  return ok * 10 >= n * 9;
}

// The output: a summary and, in detail mode, a tree of fields with byte ranges into the packet.
class Out {
 public:
  Out(const uint8_t* base, bool detail) : detail(detail), base_(base) {
    if (detail) stack_.push_back(&root);
  }
  void add(const std::string& name, const std::string& value, const uint8_t* p = nullptr,
           size_t n = 0) {
    stack_.back()->push_back(node(name, value, p, n));
  }
  void open(const std::string& name, const std::string& value, const uint8_t* p = nullptr,
            size_t n = 0) {
    json j = node(name, value, p, n);
    j["children"] = json::array();
    stack_.back()->push_back(std::move(j));
    // Only the open chain is pointed at, and nothing is appended to an ancestor while a child is
    // open, so these pointers stay valid.
    stack_.push_back(&stack_.back()->back()["children"]);
  }
  void close() {
    if (stack_.size() > 1) stack_.pop_back();
  }
  // The furthest byte a field has covered since mark(): what a parameter block's decoder did not
  // reach is shown as bytes after the fields (rest()), so a layout that ends early, or a newer
  // spec's extra fields, still show. Kept in detail mode only, like the tree.
  void mark(const uint8_t* p) { reach_ = p; }
  const uint8_t* reach() const { return reach_; }

  const bool detail;
  // The company of the packet's controller (kNoManufacturer: not known): picks the vendor
  // commands' decoder.
  uint16_t manufacturer = kNoManufacturer;
  std::string summary;
  json root = json::array();

 private:
  json node(const std::string& name, const std::string& value, const uint8_t* p, size_t n) {
    json j{{"name", name}};
    if (!value.empty()) j["value"] = value;
    if (p && n) {
      j["off"] = p - base_;
      j["len"] = n;
      if (p + n > reach_) reach_ = p + n;
    }
    return j;
  }
  const uint8_t* base_;
  const uint8_t* reach_ = nullptr;
  std::vector<json*> stack_;
};

#define F(...)                          \
  do {                                  \
    if (o.detail) o.add(__VA_ARGS__);   \
  } while (0)
#define OPEN(...)                       \
  do {                                  \
    if (o.detail) o.open(__VA_ARGS__);  \
  } while (0)
#define CLOSE()                \
  do {                         \
    if (o.detail) o.close();   \
  } while (0)
// The same for a parameter reader (Rd) in a function without an Out of its own.
#define OPEN_R(r, ...)                      \
  do {                                      \
    if ((r).o.detail) (r).o.open(__VA_ARGS__);  \
  } while (0)
#define CLOSE_R(r)                    \
  do {                                \
    if ((r).o.detail) (r).o.close();  \
  } while (0)

// ---------------------------------------------------------------------------------------------
// Name tables

const char* cmd_name(uint16_t op) {
  switch (op) {
    // Link Control
    case 0x0401: return "Inquiry";
    case 0x0402: return "Inquiry Cancel";
    case 0x0403: return "Periodic Inquiry Mode";
    case 0x0404: return "Exit Periodic Inquiry Mode";
    case 0x0405: return "Create Connection";
    case 0x0406: return "Disconnect";
    case 0x0408: return "Create Connection Cancel";
    case 0x0409: return "Accept Connection Request";
    case 0x040a: return "Reject Connection Request";
    case 0x040b: return "Link Key Request Reply";
    case 0x040c: return "Link Key Request Negative Reply";
    case 0x040d: return "PIN Code Request Reply";
    case 0x040e: return "PIN Code Request Negative Reply";
    case 0x040f: return "Change Connection Packet Type";
    case 0x0411: return "Authentication Requested";
    case 0x0413: return "Set Connection Encryption";
    case 0x0415: return "Change Connection Link Key";
    case 0x0419: return "Remote Name Request";
    case 0x041a: return "Remote Name Request Cancel";
    case 0x041b: return "Read Remote Supported Features";
    case 0x041c: return "Read Remote Extended Features";
    case 0x041d: return "Read Remote Version Information";
    case 0x041f: return "Read Clock Offset";
    case 0x0420: return "Read LMP Handle";
    case 0x0428: return "Setup Synchronous Connection";
    case 0x0429: return "Accept Synchronous Connection Request";
    case 0x042a: return "Reject Synchronous Connection Request";
    case 0x042b: return "IO Capability Request Reply";
    case 0x042c: return "User Confirmation Request Reply";
    case 0x042d: return "User Confirmation Request Negative Reply";
    case 0x042e: return "User Passkey Request Reply";
    case 0x042f: return "User Passkey Request Negative Reply";
    case 0x0430: return "Remote OOB Data Request Reply";
    case 0x0433: return "Remote OOB Data Request Negative Reply";
    case 0x0434: return "IO Capability Request Negative Reply";
    case 0x043d: return "Enhanced Setup Synchronous Connection";
    case 0x043e: return "Enhanced Accept Synchronous Connection Request";
    // Link Policy
    case 0x0801: return "Hold Mode";
    case 0x0803: return "Sniff Mode";
    case 0x0804: return "Exit Sniff Mode";
    case 0x0807: return "QoS Setup";
    case 0x0809: return "Role Discovery";
    case 0x080b: return "Switch Role";
    case 0x080c: return "Read Link Policy Settings";
    case 0x080d: return "Write Link Policy Settings";
    case 0x080e: return "Read Default Link Policy Settings";
    case 0x080f: return "Write Default Link Policy Settings";
    case 0x0810: return "Flow Specification";
    case 0x0811: return "Sniff Subrating";
    // Controller & Baseband
    case 0x0c01: return "Set Event Mask";
    case 0x0c03: return "Reset";
    case 0x0c05: return "Set Event Filter";
    case 0x0c08: return "Flush";
    case 0x0c09: return "Read PIN Type";
    case 0x0c0a: return "Write PIN Type";
    case 0x0c0d: return "Read Stored Link Key";
    case 0x0c11: return "Write Stored Link Key";
    case 0x0c12: return "Delete Stored Link Key";
    case 0x0c13: return "Write Local Name";
    case 0x0c14: return "Read Local Name";
    case 0x0c15: return "Read Connection Accept Timeout";
    case 0x0c16: return "Write Connection Accept Timeout";
    case 0x0c17: return "Read Page Timeout";
    case 0x0c18: return "Write Page Timeout";
    case 0x0c19: return "Read Scan Enable";
    case 0x0c1a: return "Write Scan Enable";
    case 0x0c1b: return "Read Page Scan Activity";
    case 0x0c1c: return "Write Page Scan Activity";
    case 0x0c1d: return "Read Inquiry Scan Activity";
    case 0x0c1e: return "Write Inquiry Scan Activity";
    case 0x0c1f: return "Read Authentication Enable";
    case 0x0c20: return "Write Authentication Enable";
    case 0x0c23: return "Read Class of Device";
    case 0x0c24: return "Write Class of Device";
    case 0x0c25: return "Read Voice Setting";
    case 0x0c26: return "Write Voice Setting";
    case 0x0c27: return "Read Automatic Flush Timeout";
    case 0x0c28: return "Write Automatic Flush Timeout";
    case 0x0c2d: return "Read Transmit Power Level";
    case 0x0c2e: return "Read Synchronous Flow Control Enable";
    case 0x0c2f: return "Write Synchronous Flow Control Enable";
    case 0x0c31: return "Set Controller To Host Flow Control";
    case 0x0c33: return "Host Buffer Size";
    case 0x0c35: return "Host Number of Completed Packets";
    case 0x0c36: return "Read Link Supervision Timeout";
    case 0x0c37: return "Write Link Supervision Timeout";
    case 0x0c38: return "Read Number of Supported IAC";
    case 0x0c39: return "Read Current IAC LAP";
    case 0x0c3a: return "Write Current IAC LAP";
    case 0x0c3f: return "Set AFH Host Channel Classification";
    case 0x0c42: return "Read Inquiry Scan Type";
    case 0x0c43: return "Write Inquiry Scan Type";
    case 0x0c44: return "Read Inquiry Mode";
    case 0x0c45: return "Write Inquiry Mode";
    case 0x0c46: return "Read Page Scan Type";
    case 0x0c47: return "Write Page Scan Type";
    case 0x0c48: return "Read AFH Channel Assessment Mode";
    case 0x0c49: return "Write AFH Channel Assessment Mode";
    case 0x0c51: return "Read Extended Inquiry Response";
    case 0x0c52: return "Write Extended Inquiry Response";
    case 0x0c53: return "Refresh Encryption Key";
    case 0x0c55: return "Read Simple Pairing Mode";
    case 0x0c56: return "Write Simple Pairing Mode";
    case 0x0c57: return "Read Local OOB Data";
    case 0x0c58: return "Read Inquiry Response TX Power Level";
    case 0x0c59: return "Write Inquiry Transmit Power Level";
    case 0x0c5a: return "Read Default Erroneous Data Reporting";
    case 0x0c5b: return "Write Default Erroneous Data Reporting";
    case 0x0c5f: return "Enhanced Flush";
    case 0x0c63: return "Set Event Mask Page 2";
    case 0x0c66: return "Read Flow Control Mode";
    case 0x0c67: return "Write Flow Control Mode";
    case 0x0c6c: return "Read LE Host Supported";
    case 0x0c6d: return "Write LE Host Supported";
    case 0x0c79: return "Read Secure Connections Host Support";
    case 0x0c7a: return "Write Secure Connections Host Support";
    case 0x0c7b: return "Read Authenticated Payload Timeout";
    case 0x0c7c: return "Write Authenticated Payload Timeout";
    case 0x0c7d: return "Read Local OOB Extended Data";
    case 0x0c77: return "Read Synchronization Train Parameters";
    case 0x0c83: return "Configure Data Path";
    // Informational
    case 0x1001: return "Read Local Version Information";
    case 0x1002: return "Read Local Supported Commands";
    case 0x1003: return "Read Local Supported Features";
    case 0x1004: return "Read Local Extended Features";
    case 0x1005: return "Read Buffer Size";
    case 0x1009: return "Read BD ADDR";
    case 0x100a: return "Read Data Block Size";
    case 0x100b: return "Read Local Supported Codecs";
    case 0x100c: return "Read Local Simple Pairing Options";
    case 0x100d: return "Read Local Supported Codecs v2";
    case 0x100e: return "Read Local Supported Codec Capabilities";
    case 0x100f: return "Read Local Supported Controller Delay";
    case 0x140c: return "Get MWS Transport Layer Configuration";
    // Status
    case 0x1401: return "Read Failed Contact Counter";
    case 0x1402: return "Reset Failed Contact Counter";
    case 0x1403: return "Read Link Quality";
    case 0x1405: return "Read RSSI";
    case 0x1406: return "Read AFH Channel Map";
    case 0x1407: return "Read Clock";
    case 0x1408: return "Read Encryption Key Size";
    // Testing
    case 0x1801: return "Read Loopback Mode";
    case 0x1802: return "Write Loopback Mode";
    case 0x1803: return "Enable Device Under Test Mode";
    case 0x1804: return "Write Simple Pairing Debug Mode";
    // LE Controller
    case 0x2001: return "LE Set Event Mask";
    case 0x2002: return "LE Read Buffer Size";
    case 0x2003: return "LE Read Local Supported Features";
    case 0x2005: return "LE Set Random Address";
    case 0x2006: return "LE Set Advertising Parameters";
    case 0x2007: return "LE Read Advertising Channel TX Power";
    case 0x2008: return "LE Set Advertising Data";
    case 0x2009: return "LE Set Scan Response Data";
    case 0x200a: return "LE Set Advertising Enable";
    case 0x200b: return "LE Set Scan Parameters";
    case 0x200c: return "LE Set Scan Enable";
    case 0x200d: return "LE Create Connection";
    case 0x200e: return "LE Create Connection Cancel";
    case 0x200f: return "LE Read Filter Accept List Size";
    case 0x2010: return "LE Clear Filter Accept List";
    case 0x2011: return "LE Add Device To Filter Accept List";
    case 0x2012: return "LE Remove Device From Filter Accept List";
    case 0x2013: return "LE Connection Update";
    case 0x2014: return "LE Set Host Channel Classification";
    case 0x2015: return "LE Read Channel Map";
    case 0x2016: return "LE Read Remote Features";
    case 0x2017: return "LE Encrypt";
    case 0x2018: return "LE Rand";
    case 0x2019: return "LE Enable Encryption";
    case 0x201a: return "LE Long Term Key Request Reply";
    case 0x201b: return "LE Long Term Key Request Negative Reply";
    case 0x201c: return "LE Read Supported States";
    case 0x201d: return "LE Receiver Test";
    case 0x201e: return "LE Transmitter Test";
    case 0x201f: return "LE Test End";
    case 0x2020: return "LE Remote Connection Parameter Request Reply";
    case 0x2021: return "LE Remote Connection Parameter Request Negative Reply";
    case 0x2022: return "LE Set Data Length";
    case 0x2023: return "LE Read Suggested Default Data Length";
    case 0x2024: return "LE Write Suggested Default Data Length";
    case 0x2025: return "LE Read Local P-256 Public Key";
    case 0x2026: return "LE Generate DHKey";
    case 0x2027: return "LE Add Device To Resolving List";
    case 0x2028: return "LE Remove Device From Resolving List";
    case 0x2029: return "LE Clear Resolving List";
    case 0x202a: return "LE Read Resolving List Size";
    case 0x202b: return "LE Read Peer Resolvable Address";
    case 0x202c: return "LE Read Local Resolvable Address";
    case 0x202d: return "LE Set Address Resolution Enable";
    case 0x202e: return "LE Set Resolvable Private Address Timeout";
    case 0x202f: return "LE Read Maximum Data Length";
    case 0x2030: return "LE Read PHY";
    case 0x2031: return "LE Set Default PHY";
    case 0x2032: return "LE Set PHY";
    case 0x2035: return "LE Set Advertising Set Random Address";
    case 0x2036: return "LE Set Extended Advertising Parameters";
    case 0x2037: return "LE Set Extended Advertising Data";
    case 0x2038: return "LE Set Extended Scan Response Data";
    case 0x2039: return "LE Set Extended Advertising Enable";
    case 0x203a: return "LE Read Maximum Advertising Data Length";
    case 0x203b: return "LE Read Number of Supported Advertising Sets";
    case 0x203c: return "LE Remove Advertising Set";
    case 0x203d: return "LE Clear Advertising Sets";
    case 0x203e: return "LE Set Periodic Advertising Parameters";
    case 0x203f: return "LE Set Periodic Advertising Data";
    case 0x2040: return "LE Set Periodic Advertising Enable";
    case 0x2041: return "LE Set Extended Scan Parameters";
    case 0x2042: return "LE Set Extended Scan Enable";
    case 0x2043: return "LE Extended Create Connection";
    case 0x2044: return "LE Periodic Advertising Create Sync";
    case 0x2045: return "LE Periodic Advertising Create Sync Cancel";
    case 0x2046: return "LE Periodic Advertising Terminate Sync";
    case 0x2047: return "LE Add Device To Periodic Advertiser List";
    case 0x2048: return "LE Remove Device From Periodic Advertiser List";
    case 0x2049: return "LE Clear Periodic Advertiser List";
    case 0x204a: return "LE Read Periodic Advertiser List Size";
    case 0x204b: return "LE Read Transmit Power";
    case 0x204e: return "LE Set Privacy Mode";
    case 0x204c: return "LE Read RF Path Compensation";
    case 0x204d: return "LE Write RF Path Compensation";
    case 0x2059: return "LE Set Periodic Advertising Receive Enable";
    case 0x205a: return "LE Periodic Advertising Sync Transfer";
    case 0x205b: return "LE Periodic Advertising Set Info Transfer";
    case 0x205c: return "LE Set Periodic Advertising Sync Transfer Parameters";
    case 0x205d: return "LE Set Default Periodic Advertising Sync Transfer Parameters";
    case 0x205e: return "LE Generate DHKey v2";
    case 0x205f: return "LE Modify Sleep Clock Accuracy";
    case 0x2060: return "LE Read Buffer Size v2";
    case 0x2061: return "LE Read ISO TX Sync";
    case 0x2062: return "LE Set CIG Parameters";
    case 0x2063: return "LE Set CIG Parameters Test";
    case 0x2064: return "LE Create CIS";
    case 0x2065: return "LE Remove CIG";
    case 0x2066: return "LE Accept CIS Request";
    case 0x2067: return "LE Reject CIS Request";
    case 0x2068: return "LE Create BIG";
    case 0x2069: return "LE Create BIG Test";
    case 0x206a: return "LE Terminate BIG";
    case 0x206b: return "LE BIG Create Sync";
    case 0x206c: return "LE BIG Terminate Sync";
    case 0x206d: return "LE Request Peer SCA";
    case 0x206e: return "LE Setup ISO Data Path";
    case 0x206f: return "LE Remove ISO Data Path";
    case 0x2074: return "LE Set Host Feature";
    case 0x2075: return "LE Read ISO Link Quality";
    case 0x2070: return "LE ISO Transmit Test";
    case 0x2071: return "LE ISO Receive Test";
    case 0x2072: return "LE ISO Read Test Counters";
    case 0x2073: return "LE ISO Test End";
    case 0x2076: return "LE Enhanced Read Transmit Power Level";
    case 0x2077: return "LE Read Remote Transmit Power Level";
    case 0x2078: return "LE Set Path Loss Reporting Parameters";
    case 0x2079: return "LE Set Path Loss Reporting Enable";
    case 0x207a: return "LE Set Transmit Power Reporting Enable";
    case 0x207b: return "LE Transmitter Test v4";
    case 0x207c: return "LE Set Data Related Address Changes";
    case 0x207d: return "LE Set Default Subrate";
    case 0x207e: return "LE Subrate Request";
    case 0x207f: return "LE Set Extended Advertising Parameters v2";
    case 0x2082: return "LE Set Periodic Advertising Subevent Data";
    case 0x2083: return "LE Set Periodic Advertising Response Data";
    case 0x2084: return "LE Set Periodic Sync Subevent";
    case 0x2085: return "LE Extended Create Connection v2";
    case 0x2086: return "LE Set Periodic Advertising Parameters v2";
    default: return nullptr;
  }
}

// Vendor opcodes (OGF 0x3f) mean one thing on an Intel controller and another on a Broadcom or
// Realtek one, so they are Intel's only where the controller said it is Intel (Out::manufacturer).
constexpr uint16_t kCompanyIntel = 0x0002;
// Intel's opcode for the Microsoft extension (btintel_set_msft_opcode), which bluetoothd's
// advertisement monitors go through.
constexpr uint16_t kIntelMsft = 0xfc1e;

bool intel_vendor(uint16_t manufacturer, uint16_t op) { return manufacturer == kCompanyIntel && (op >> 10) == 0x3f; }

// Named as btmon's monitor/intel.c names them; the ones it leaves unnamed (0xfc0a, 0xfc4e, 0xfc86,
// 0xfca1, 0xfcb3) as the kernel's btintel.c uses them.
const char* intel_cmd_name(uint16_t op) {
  switch (op) {
    case 0xfc01: return "Intel Reset";
    case 0xfc02: return "Intel No Operation";
    case 0xfc05: return "Intel Read Version";
    case 0xfc06: return "Intel Set UART Baudrate";
    case 0xfc07: return "Intel Enable LPM";
    case 0xfc08: return "Intel PCM Write Configuration";
    case 0xfc09: return "Intel Secure Send";
    case 0xfc0a: return "Intel Set DSBR";
    case 0xfc0d: return "Intel Read Secure Boot Params";
    case 0xfc0e: return "Intel Write Secure Boot Params";
    case 0xfc0f: return "Intel Unlock";
    case 0xfc10: return "Intel Change UART Baudrate";
    case 0xfc11: return "Intel Manufacturer Mode";
    case 0xfc12: return "Intel Read Link RSSI";
    case kIntelMsft: return "MSFT Extension";
    case 0xfc22: return "Intel Get Exception Info";
    case 0xfc24: return "Intel Clear Exception Info";
    case 0xfc2f: return "Intel Write BD Data";
    case 0xfc30: return "Intel Read BD Data";
    case 0xfc31: return "Intel Write BD Address";
    case 0xfc32: return "Intel Flow Specification";
    case 0xfc34: return "Intel Read Secure ID";
    case 0xfc38: return "Intel Set Synchronous USB Interface Type";
    case 0xfc39: return "Intel Config Synchronous Interface";
    case 0xfc3f: return "Intel SW RF Kill";
    case 0xfc43: return "Intel Activate Deactivate Traces";
    case 0xfc4d: return "Intel Stimulate Exception";
    case 0xfc4e: return "Intel Coredump";
    case 0xfc50: return "Intel Read HW Version";
    case 0xfc52: return "Intel Set Event Mask";
    case 0xfc53: return "Intel Config Link Controller";
    case 0xfc86: return "Intel Read Offload Use Cases";
    case 0xfc89: return "Intel DDC Write";
    case 0xfc8a: return "Intel DDC Read";
    case 0xfc8b: return "Intel DDC Config Write";
    case 0xfc8c: return "Intel DDC Config Read";
    case 0xfc8d: return "Intel Memory Read";
    case 0xfc8e: return "Intel Memory Write";
    case 0xfca1: return "Intel Set Link Statistics Traces";
    case 0xfca6: return "Intel Read Supported Features";
    case 0xfcb3: return "Intel Read FSEQ Status";
    case 0xfe0b: return "Intel PPAG Enable";
    default: return nullptr;
  }
}

std::string cmd_label(uint16_t op, uint16_t manufacturer) {
  if (const char* n = cmd_name(op)) return n;
  if ((op >> 10) == 0x3f) {
    if (intel_vendor(manufacturer, op)) {
      if (const char* n = intel_cmd_name(op)) return n;
      return sf("Intel 0x%04x", op);
    }
    return sf("Vendor 0x%04x", op);
  }
  return sf("Command 0x%04x", op);
}

const char* evt_name(uint8_t code) {
  switch (code) {
    case 0x01: return "Inquiry Complete";
    case 0x02: return "Inquiry Result";
    case 0x03: return "Connection Complete";
    case 0x04: return "Connection Request";
    case 0x05: return "Disconnection Complete";
    case 0x06: return "Authentication Complete";
    case 0x07: return "Remote Name Request Complete";
    case 0x08: return "Encryption Change";
    case 0x09: return "Change Connection Link Key Complete";
    case 0x0a: return "Link Key Type Changed";
    case 0x0b: return "Read Remote Supported Features Complete";
    case 0x0c: return "Read Remote Version Information Complete";
    case 0x0d: return "QoS Setup Complete";
    case 0x0e: return "Command Complete";
    case 0x0f: return "Command Status";
    case 0x10: return "Hardware Error";
    case 0x11: return "Flush Occurred";
    case 0x12: return "Role Change";
    case 0x13: return "Number of Completed Packets";
    case 0x14: return "Mode Change";
    case 0x15: return "Return Link Keys";
    case 0x16: return "PIN Code Request";
    case 0x17: return "Link Key Request";
    case 0x18: return "Link Key Notification";
    case 0x19: return "Loopback Command";
    case 0x1a: return "Data Buffer Overflow";
    case 0x1b: return "Max Slots Change";
    case 0x1c: return "Read Clock Offset Complete";
    case 0x1d: return "Connection Packet Type Changed";
    case 0x1e: return "QoS Violation";
    case 0x20: return "Page Scan Repetition Mode Change";
    case 0x21: return "Flow Specification Complete";
    case 0x22: return "Inquiry Result with RSSI";
    case 0x23: return "Read Remote Extended Features Complete";
    case 0x2c: return "Synchronous Connection Complete";
    case 0x2d: return "Synchronous Connection Changed";
    case 0x2e: return "Sniff Subrating";
    case 0x2f: return "Extended Inquiry Result";
    case 0x30: return "Encryption Key Refresh Complete";
    case 0x31: return "IO Capability Request";
    case 0x32: return "IO Capability Response";
    case 0x33: return "User Confirmation Request";
    case 0x34: return "User Passkey Request";
    case 0x35: return "Remote OOB Data Request";
    case 0x36: return "Simple Pairing Complete";
    case 0x38: return "Link Supervision Timeout Changed";
    case 0x39: return "Enhanced Flush Complete";
    case 0x3b: return "User Passkey Notification";
    case 0x3c: return "Keypress Notification";
    case 0x3d: return "Remote Host Supported Features Notification";
    case 0x3e: return "LE Meta Event";
    case 0x48: return "Number of Completed Data Blocks";
    case 0x57: return "Authenticated Payload Timeout Expired";
    case 0x59: return "Encryption Change v2";
    case 0xff: return "Vendor";
    default: return nullptr;
  }
}

const char* le_sub_name(uint8_t s) {
  switch (s) {
    case 0x01: return "LE Connection Complete";
    case 0x02: return "LE Advertising Report";
    case 0x03: return "LE Connection Update Complete";
    case 0x04: return "LE Read Remote Features Complete";
    case 0x05: return "LE Long Term Key Request";
    case 0x06: return "LE Remote Connection Parameter Request";
    case 0x07: return "LE Data Length Change";
    case 0x08: return "LE Read Local P-256 Public Key Complete";
    case 0x09: return "LE Generate DHKey Complete";
    case 0x0a: return "LE Enhanced Connection Complete";
    case 0x0b: return "LE Directed Advertising Report";
    case 0x0c: return "LE PHY Update Complete";
    case 0x0d: return "LE Extended Advertising Report";
    case 0x0e: return "LE Periodic Advertising Sync Established";
    case 0x0f: return "LE Periodic Advertising Report";
    case 0x10: return "LE Periodic Advertising Sync Lost";
    case 0x11: return "LE Scan Timeout";
    case 0x12: return "LE Advertising Set Terminated";
    case 0x13: return "LE Scan Request Received";
    case 0x14: return "LE Channel Selection Algorithm";
    case 0x19: return "LE CIS Established";
    case 0x1a: return "LE CIS Request";
    case 0x1b: return "LE Create BIG Complete";
    case 0x1c: return "LE Terminate BIG Complete";
    case 0x1d: return "LE BIG Sync Established";
    case 0x1e: return "LE BIG Sync Lost";
    case 0x15: return "LE Connectionless IQ Report";
    case 0x16: return "LE Connection IQ Report";
    case 0x17: return "LE CTE Request Failed";
    case 0x18: return "LE Periodic Advertising Sync Transfer Received";
    case 0x1f: return "LE Request Peer SCA Complete";
    case 0x20: return "LE Path Loss Threshold";
    case 0x21: return "LE Transmit Power Reporting";
    case 0x22: return "LE BIGInfo Advertising Report";
    case 0x23: return "LE Subrate Change";
    case 0x24: return "LE Periodic Advertising Sync Established v2";
    case 0x25: return "LE Periodic Advertising Report v2";
    case 0x26: return "LE Periodic Advertising Sync Transfer Received v2";
    case 0x27: return "LE Periodic Advertising Subevent Data Request";
    case 0x28: return "LE Periodic Advertising Response Report";
    case 0x29: return "LE Enhanced Connection Complete v2";
    case 0x2a: return "LE CIS Established v2";
    default: return nullptr;
  }
}

const char* error_name(uint8_t e) {
  switch (e) {
    case 0x00: return "Success";
    case 0x01: return "Unknown HCI Command";
    case 0x02: return "Unknown Connection Identifier";
    case 0x03: return "Hardware Failure";
    case 0x04: return "Page Timeout";
    case 0x05: return "Authentication Failure";
    case 0x06: return "PIN or Key Missing";
    case 0x07: return "Memory Capacity Exceeded";
    case 0x08: return "Connection Timeout";
    case 0x09: return "Connection Limit Exceeded";
    case 0x0a: return "Synchronous Connection Limit Exceeded";
    case 0x0b: return "Connection Already Exists";
    case 0x0c: return "Command Disallowed";
    case 0x0d: return "Connection Rejected (Limited Resources)";
    case 0x0e: return "Connection Rejected (Security Reasons)";
    case 0x0f: return "Connection Rejected (Unacceptable BD_ADDR)";
    case 0x10: return "Connection Accept Timeout Exceeded";
    case 0x11: return "Unsupported Feature or Parameter Value";
    case 0x12: return "Invalid HCI Command Parameters";
    case 0x13: return "Remote User Terminated Connection";
    case 0x14: return "Remote Device Terminated (Low Resources)";
    case 0x15: return "Remote Device Terminated (Power Off)";
    case 0x16: return "Connection Terminated By Local Host";
    case 0x17: return "Repeated Attempts";
    case 0x18: return "Pairing Not Allowed";
    case 0x19: return "Unknown LMP PDU";
    case 0x1a: return "Unsupported Remote Feature";
    case 0x1b: return "SCO Offset Rejected";
    case 0x1c: return "SCO Interval Rejected";
    case 0x1d: return "SCO Air Mode Rejected";
    case 0x1e: return "Invalid LMP/LL Parameters";
    case 0x1f: return "Unspecified Error";
    case 0x20: return "Unsupported LMP/LL Parameter Value";
    case 0x21: return "Role Change Not Allowed";
    case 0x22: return "LMP/LL Response Timeout";
    case 0x23: return "LMP Error Transaction Collision";
    case 0x24: return "LMP PDU Not Allowed";
    case 0x25: return "Encryption Mode Not Acceptable";
    case 0x26: return "Link Key Cannot Be Changed";
    case 0x27: return "Requested QoS Not Supported";
    case 0x28: return "Instant Passed";
    case 0x29: return "Pairing With Unit Key Not Supported";
    case 0x2a: return "Different Transaction Collision";
    case 0x2c: return "QoS Unacceptable Parameter";
    case 0x2d: return "QoS Rejected";
    case 0x2e: return "Channel Classification Not Supported";
    case 0x2f: return "Insufficient Security";
    case 0x30: return "Parameter Out Of Mandatory Range";
    case 0x32: return "Role Switch Pending";
    case 0x34: return "Reserved Slot Violation";
    case 0x35: return "Role Switch Failed";
    case 0x36: return "Extended Inquiry Response Too Large";
    case 0x37: return "Secure Simple Pairing Not Supported By Host";
    case 0x38: return "Host Busy - Pairing";
    case 0x39: return "Connection Rejected (No Suitable Channel)";
    case 0x3a: return "Controller Busy";
    case 0x3b: return "Unacceptable Connection Parameters";
    case 0x3c: return "Advertising Timeout";
    case 0x3d: return "Connection Terminated (MIC Failure)";
    case 0x3e: return "Connection Failed To Be Established";
    case 0x3f: return "MAC Connection Failed";
    case 0x40: return "Coarse Clock Adjustment Rejected";
    case 0x41: return "Type0 Submap Not Defined";
    case 0x42: return "Unknown Advertising Identifier";
    case 0x43: return "Limit Reached";
    case 0x44: return "Operation Cancelled By Host";
    case 0x45: return "Packet Too Long";
    default: return "Unknown";
  }
}

std::string status_str(uint8_t s) { return sf("0x%02x (%s)", s, error_name(s)); }

const char* att_op_name(uint8_t op) {
  switch (op) {
    case 0x01: return "Error Response";
    case 0x02: return "Exchange MTU Request";
    case 0x03: return "Exchange MTU Response";
    case 0x04: return "Find Information Request";
    case 0x05: return "Find Information Response";
    case 0x06: return "Find By Type Value Request";
    case 0x07: return "Find By Type Value Response";
    case 0x08: return "Read By Type Request";
    case 0x09: return "Read By Type Response";
    case 0x0a: return "Read Request";
    case 0x0b: return "Read Response";
    case 0x0c: return "Read Blob Request";
    case 0x0d: return "Read Blob Response";
    case 0x0e: return "Read Multiple Request";
    case 0x0f: return "Read Multiple Response";
    case 0x10: return "Read By Group Type Request";
    case 0x11: return "Read By Group Type Response";
    case 0x12: return "Write Request";
    case 0x13: return "Write Response";
    case 0x16: return "Prepare Write Request";
    case 0x17: return "Prepare Write Response";
    case 0x18: return "Execute Write Request";
    case 0x19: return "Execute Write Response";
    case 0x1b: return "Handle Value Notification";
    case 0x1d: return "Handle Value Indication";
    case 0x1e: return "Handle Value Confirmation";
    case 0x20: return "Read Multiple Variable Request";
    case 0x21: return "Read Multiple Variable Response";
    case 0x23: return "Multiple Handle Value Notification";
    case 0x52: return "Write Command";
    case 0xd2: return "Signed Write Command";
    default: return nullptr;
  }
}

const char* att_error_name(uint8_t e) {
  switch (e) {
    case 0x01: return "Invalid Handle";
    case 0x02: return "Read Not Permitted";
    case 0x03: return "Write Not Permitted";
    case 0x04: return "Invalid PDU";
    case 0x05: return "Insufficient Authentication";
    case 0x06: return "Request Not Supported";
    case 0x07: return "Invalid Offset";
    case 0x08: return "Insufficient Authorization";
    case 0x09: return "Prepare Queue Full";
    case 0x0a: return "Attribute Not Found";
    case 0x0b: return "Attribute Not Long";
    case 0x0c: return "Encryption Key Size Too Short";
    case 0x0d: return "Invalid Attribute Value Length";
    case 0x0e: return "Unlikely Error";
    case 0x0f: return "Insufficient Encryption";
    case 0x10: return "Unsupported Group Type";
    case 0x11: return "Insufficient Resources";
    case 0x12: return "Database Out Of Sync";
    case 0x13: return "Value Not Allowed";
    case 0xfc: return "Write Request Rejected";
    case 0xfd: return "CCCD Improperly Configured";
    case 0xfe: return "Procedure Already in Progress";
    case 0xff: return "Out of Range";
    default: return "Application Error";
  }
}

const char* uuid16_name(uint16_t u) {
  switch (u) {
    case 0x0001: return "SDP";
    case 0x0003: return "RFCOMM";
    case 0x0017: return "AVCTP";
    case 0x0008: return "OBEX";
    case 0x000f: return "BNEP";
    case 0x0011: return "HIDP";
    case 0x0019: return "AVDTP";
    case 0x0100: return "L2CAP";
    case 0x1000: return "Service Discovery Server";
    case 0x1002: return "Public Browse Root";
    case 0x1101: return "Serial Port";
    case 0x1103: return "Dialup Networking";
    case 0x1105: return "OBEX Object Push";
    case 0x1106: return "OBEX File Transfer";
    case 0x1108: return "Headset";
    case 0x110a: return "Audio Source";
    case 0x110b: return "Audio Sink";
    case 0x110c: return "A/V Remote Control Target";
    case 0x110d: return "Advanced Audio Distribution";
    case 0x110e: return "A/V Remote Control";
    case 0x110f: return "A/V Remote Control Controller";
    case 0x1112: return "Headset AG";
    case 0x1115: return "PANU";
    case 0x1116: return "NAP";
    case 0x1117: return "GN";
    case 0x1124: return "Human Interface Device Service";
    case 0x112d: return "SIM Access";
    case 0x112e: return "Phonebook Access PCE";
    case 0x111e: return "Handsfree";
    case 0x111f: return "Handsfree Audio Gateway";
    case 0x112f: return "Phonebook Access PSE";
    case 0x1130: return "Phonebook Access";
    case 0x1131: return "Headset HS";
    case 0x1132: return "Message Access Server";
    case 0x1133: return "Message Notification Server";
    case 0x1134: return "Message Access Profile";
    case 0x1200: return "PnP Information";
    case 0x1201: return "Generic Networking";
    case 0x1203: return "Generic Audio";
    case 0x1204: return "Generic Telephony";
    case 0x1800: return "Generic Access";
    case 0x1801: return "Generic Attribute";
    case 0x180a: return "Device Information";
    case 0x180d: return "Heart Rate";
    case 0x180f: return "Battery";
    case 0x1812: return "Human Interface Device";
    case 0x184e: return "Audio Stream Control";
    case 0x184f: return "Broadcast Audio Scan";
    case 0x1850: return "Published Audio Capabilities";
    case 0x1844: return "Volume Control";
    case 0x1843: return "Audio Input Control";
    case 0x1845: return "Volume Offset Control";
    case 0x1846: return "Coordinated Set Identification";
    case 0x1848: return "Media Control";
    case 0x1849: return "Generic Media Control";
    case 0x184d: return "Microphone Control";
    case 0x1853: return "Common Audio";
    case 0x1854: return "Hearing Access";
    case 0x1855: return "Telephony and Media Audio";
    case 0x1856: return "Public Broadcast Announcement";
    case 0x184b: return "Telephone Bearer";
    case 0x184c: return "Generic Telephone Bearer";
    case 0x1851: return "Basic Audio Announcement";
    case 0x1852: return "Broadcast Audio Announcement";
    case 0x1858: return "Gaming Audio";
    case 0x2800: return "Primary Service";
    case 0x2801: return "Secondary Service";
    case 0x2802: return "Include";
    case 0x2803: return "Characteristic";
    case 0x2900: return "Characteristic Extended Properties";
    case 0x2901: return "Characteristic User Description";
    case 0x2902: return "Client Characteristic Configuration";
    case 0x2a00: return "Device Name";
    case 0x2a01: return "Appearance";
    case 0x2a04: return "Peripheral Preferred Connection Parameters";
    case 0x2a05: return "Service Changed";
    case 0x2a19: return "Battery Level";
    case 0x2a23: return "System ID";
    case 0x2a24: return "Model Number String";
    case 0x2a25: return "Serial Number String";
    case 0x2a26: return "Firmware Revision String";
    case 0x2a27: return "Hardware Revision String";
    case 0x2a28: return "Software Revision String";
    case 0x2a29: return "Manufacturer Name String";
    case 0x2a4a: return "HID Information";
    case 0x2a4b: return "Report Map";
    case 0x2a4c: return "HID Control Point";
    case 0x2a4e: return "Protocol Mode";
    case 0x2a50: return "PnP ID";
    case 0x2908: return "Report Reference";
    case 0x2a37: return "Heart Rate Measurement";
    case 0x2a4d: return "Report";
    case 0x2aa6: return "Central Address Resolution";
    case 0x2b29: return "Client Supported Features";
    case 0x2b2a: return "Database Hash";
    case 0x2b3a: return "Server Supported Features";
    case 0x2b7d: return "Volume State";
    case 0x2b7e: return "Volume Control Point";
    case 0x2b7f: return "Volume Flags";
    case 0x2ac9: return "Resolvable Private Address Only";
    case 0x2b51: return "TMAP Role";
    case 0x2b77: return "Audio Input State";
    case 0x2b78: return "Gain Setting Attributes";
    case 0x2b79: return "Audio Input Type";
    case 0x2b7a: return "Audio Input Status";
    case 0x2b7b: return "Audio Input Control Point";
    case 0x2b7c: return "Audio Input Description";
    case 0x2b80: return "Volume Offset State";
    case 0x2b81: return "Audio Location";
    case 0x2b82: return "Volume Offset Control Point";
    case 0x2b83: return "Audio Output Description";
    case 0x2b84: return "Set Identity Resolving Key";
    case 0x2b85: return "Coordinated Set Size";
    case 0x2b86: return "Set Member Lock";
    case 0x2b87: return "Set Member Rank";
    case 0x2b93: return "Media Player Name";
    case 0x2b94: return "Media Player Icon Object ID";
    case 0x2b95: return "Media Player Icon URL";
    case 0x2b96: return "Track Changed";
    case 0x2b97: return "Track Title";
    case 0x2b98: return "Track Duration";
    case 0x2b99: return "Track Position";
    case 0x2b9a: return "Playback Speed";
    case 0x2b9b: return "Seeking Speed";
    case 0x2b9c: return "Current Track Segments Object ID";
    case 0x2b9d: return "Current Track Object ID";
    case 0x2b9e: return "Next Track Object ID";
    case 0x2b9f: return "Parent Group Object ID";
    case 0x2ba0: return "Current Group Object ID";
    case 0x2ba1: return "Playing Order";
    case 0x2ba2: return "Playing Orders Supported";
    case 0x2ba3: return "Media State";
    case 0x2ba4: return "Media Control Point";
    case 0x2ba5: return "Media Control Point Opcodes Supported";
    case 0x2ba6: return "Search Results Object ID";
    case 0x2ba7: return "Search Control Point";
    case 0x2bb3: return "Bearer Provider Name";
    case 0x2bb4: return "Bearer UCI";
    case 0x2bb5: return "Bearer Technology";
    case 0x2bb6: return "Bearer URI Schemes Supported";
    case 0x2bb7: return "Bearer Signal Strength";
    case 0x2bb8: return "Bearer Signal Strength Reporting Interval";
    case 0x2bb9: return "Bearer List Current Calls";
    case 0x2bba: return "Content Control ID";
    case 0x2bbb: return "Status Flags";
    case 0x2bbc: return "Incoming Call Target Bearer URI";
    case 0x2bbd: return "Call State";
    case 0x2bbe: return "Call Control Point";
    case 0x2bbf: return "Call Control Point Optional Opcodes";
    case 0x2bc0: return "Termination Reason";
    case 0x2bc1: return "Incoming Call";
    case 0x2bc2: return "Call Friendly Name";
    case 0x2bc3: return "Mute";
    case 0x2bc7: return "Broadcast Audio Scan Control Point";
    case 0x2bc8: return "Broadcast Receive State";
    case 0x2bda: return "Hearing Aid Features";
    case 0x2bdb: return "Hearing Aid Preset Control Point";
    case 0x2bdc: return "Active Preset Index";
    case 0x2bc4: return "Sink ASE";
    case 0x2bc5: return "Source ASE";
    case 0x2bc6: return "ASE Control Point";
    case 0x2bc9: return "Sink PAC";
    case 0x2bca: return "Sink Audio Locations";
    case 0x2bcb: return "Source PAC";
    case 0x2bcc: return "Source Audio Locations";
    case 0x2bcd: return "Available Audio Contexts";
    case 0x2bce: return "Supported Audio Contexts";
    default: return nullptr;
  }
}

std::string uuid_str(const uint8_t* p, size_t n) {
  if (n == 2) {
    const uint16_t u = le16(p);
    const char* name = uuid16_name(u);
    return name ? sf("0x%04x (%s)", u, name) : sf("0x%04x", u);
  }
  if (n == 4) return sf("0x%08x", le32(p));
  if (n == 16) {
    // Little-endian on the wire, printed in the usual big-endian groups.
    return sf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", p[15], p[14],
              p[13], p[12], p[11], p[10], p[9], p[8], p[7], p[6], p[5], p[4], p[3], p[2], p[1],
              p[0]);
  }
  return hexbytes(p, n);
}

const char* addr_type_name(uint8_t t) {
  switch (t) {
    case 0x00: return "public";
    case 0x01: return "random";
    case 0x02: return "public identity";
    case 0x03: return "random identity";
    default: return "unknown";
  }
}

// The address a controller uses for itself (Own_Address_Type): 2 and 3 ask it to resolve.
const char* own_addr_type_name(uint8_t t) {
  switch (t) {
    case 0x00: return "public";
    case 0x01: return "random";
    case 0x02: return "RPA, else public";
    case 0x03: return "RPA, else random";
    default: return "unknown";
  }
}

const char* phy_name(uint8_t p) {
  switch (p) {
    case 0x01: return "LE 1M";
    case 0x02: return "LE 2M";
    case 0x03: return "LE Coded";
    case 0x04: return "LE Coded S=2";  // secondary advertising PHY options of v2 commands
    default: return "?";
  }
}

// The set bits of v by name, unknown ones as "bit N": "connectable, scannable, legacy".
std::string bit_names(uint64_t v, const char* const* names, size_t count) {
  std::string s;
  for (unsigned b = 0; b < 64; ++b) {
    if (!(v >> b & 1)) continue;
    if (!s.empty()) s += ", ";
    s += b < count && names[b] ? std::string(names[b]) : sf("bit %u", b);
  }
  return s.empty() ? "none" : s;
}

const char* const kPhyBits[] = {"LE 1M", "LE 2M", "LE Coded"};
std::string phys_str(uint8_t v) { return sf("0x%02x (%s)", v, bit_names(v, kPhyBits, 3).c_str()); }

// A cursor over a parameter block, for layouts read field by field: each call puts one field in
// the tree (formatting it only in detail mode) and returns its value, 0 past the end. Most HCI
// parameters are fixed layouts of small integers, so this keeps each one to a line per field.
class Rd {
 public:
  Rd(Out& o, const uint8_t* p, size_t n) : o(o), p_(p), n_(n) {}
  bool has(size_t k) const { return at_ + k <= n_; }
  size_t left() const { return at_ < n_ ? n_ - at_ : 0; }
  const uint8_t* cur() const { return p_ + at_; }
  void skip(size_t k) { at_ = std::min(n_, at_ + k); }

  uint32_t raw(size_t k) const {
    if (!has(k)) return 0;
    uint32_t v = 0;
    for (size_t i = 0; i < k; ++i) v |= static_cast<uint32_t>(p_[at_ + i]) << (8 * i);
    return v;
  }
  // An integer of k bytes; fmt formats it (printf with one unsigned argument).
  uint32_t num(const char* name, size_t k, const char* fmt = "%u") {
    if (!has(k)) return skip(k), 0;
    const uint32_t v = raw(k);
    if (o.detail) o.add(name, sf(fmt, v), cur(), k);
    at_ += k;
    return v;
  }
  uint8_t u8(const char* name, const char* fmt = "%u") { return static_cast<uint8_t>(num(name, 1, fmt)); }
  uint16_t u16(const char* name, const char* fmt = "%u") { return static_cast<uint16_t>(num(name, 2, fmt)); }
  uint32_t u24(const char* name, const char* fmt = "%u") { return num(name, 3, fmt); }
  uint32_t u32(const char* name, const char* fmt = "%u") { return num(name, 4, fmt); }
  // A value with a text of its own: v is the integer, text its rendering.
  template <typename Fn>
  uint32_t with(const char* name, size_t k, Fn text) {
    if (!has(k)) return skip(k), 0;
    const uint32_t v = raw(k);
    if (o.detail) o.add(name, text(v), cur(), k);
    at_ += k;
    return v;
  }
  // A number from a table of names: "0x02 (LE 2M)".
  template <typename Fn>
  uint32_t named(const char* name, size_t k, Fn fn) {
    return with(name, k, [&](uint32_t v) { return sf(k == 1 ? "0x%02x (%s)" : k == 2 ? "0x%04x (%s)" : "0x%06x (%s)", v, fn(v)); });
  }
  // Bit flags: "0x0013 (connectable, scannable, legacy)".
  uint32_t flags(const char* name, size_t k, const char* const* names, size_t count) {
    return with(name, k, [&](uint32_t v) {
      return sf(k == 1 ? "0x%02x" : k == 2 ? "0x%04x" : k == 3 ? "0x%06x" : "0x%08x", v) + " (" + bit_names(v, names, count) + ")";
    });
  }
  // A time in units of unit_ms: "20.00 ms (0x0010)".
  uint32_t time(const char* name, size_t k, double unit_ms) {
    return with(name, k, [&](uint32_t v) { return sf("%.2f ms (0x%x)", v * unit_ms, v); });
  }
  int8_t dbm(const char* name) {
    return static_cast<int8_t>(with(name, 1, [](uint32_t v) {
      return v == 0x7f ? std::string("not available (0x7f)") : sf("%d dBm", static_cast<int8_t>(v));
    }));
  }
  uint16_t handle(const char* name = "Handle") {
    return static_cast<uint16_t>(with(name, 2, [](uint32_t v) { return std::to_string(v & 0x0fff); }) & 0x0fff);
  }
  uint8_t status(const char* name = "Status") {
    return static_cast<uint8_t>(with(name, 1, [](uint32_t v) { return status_str(static_cast<uint8_t>(v)); }));
  }
  uint8_t yes_no(const char* name, const char* on = "enabled", const char* off = "disabled") {
    return static_cast<uint8_t>(with(name, 1, [&](uint32_t v) { return sf("%s (0x%02x)", v ? on : off, v); }));
  }
  uint8_t atype(const char* name = "Address type") {
    return static_cast<uint8_t>(with(name, 1, [](uint32_t v) { return std::string(addr_type_name(static_cast<uint8_t>(v))); }));
  }
  uint8_t own_atype(const char* name = "Own address type") {
    return static_cast<uint8_t>(with(name, 1, [](uint32_t v) { return std::string(own_addr_type_name(static_cast<uint8_t>(v))); }));
  }
  uint8_t phy(const char* name) { return static_cast<uint8_t>(named(name, 1, [](uint32_t v) { return phy_name(static_cast<uint8_t>(v)); })); }
  uint8_t phys(const char* name) { return static_cast<uint8_t>(with(name, 1, [](uint32_t v) { return phys_str(static_cast<uint8_t>(v)); })); }
  // A BD_ADDR; returns it as text (empty past the end).
  std::string addr(const char* name = "Address") {
    if (!has(6)) return skip(6), std::string();
    const std::string a = bdaddr_str(cur());
    if (o.detail) o.add(name, a, cur(), 6);
    at_ += 6;
    return a;
  }
  void hex(const char* name, size_t k) {
    k = std::min(k, left());
    if (k && o.detail) o.add(name, hexbytes(cur(), k, 64), cur(), k);
    at_ += k;
  }

  Out& o;

 private:
  const uint8_t* p_;
  size_t n_;
  size_t at_ = 0;
};

// The bytes of [p, p + n) no field covered since o.mark(): all of them as `name` when nothing
// was decoded (a command without a layout here), else the undecoded tail.
void rest(Out& o, const uint8_t* p, size_t n, const char* name = "Parameters") {
  if (!o.detail || !n) return;
  const uint8_t* end = p + n;
  const uint8_t* r = o.reach();
  if (r && r >= end) return;
  if (!r || r <= p) {
    o.add(name, hexbytes(p, n, 64), p, n);
  } else {
    o.add("Undecoded", hexbytes(r, static_cast<size_t>(end - r), 64), r, static_cast<size_t>(end - r));
  }
}

// ---------------------------------------------------------------------------------------------
// MGMT: what bluetoothd (or btmgmt) asks the kernel and what the kernel tells it, carried on the
// monitor channel as control commands and events. Named as btmon names them (mgmt_names.h), with
// the fields read most: a mode switched on or off, the adapter settings after a change, the device
// a command or event is about and why a link went down.

const char* mgmt_addr_type(uint8_t t) {
  switch (t) {
    case 0: return "BR/EDR";
    case 1: return "LE public";
    case 2: return "LE random";
    default: return "unknown";
  }
}

std::string mgmt_settings(uint32_t v) {
  static const char* const kBits[] = {"powered", "connectable", "fast-connectable", "discoverable", "bondable",
                                      "link-security", "ssp", "br/edr", "hs", "le", "advertising", "secure-conn",
                                      "debug-keys", "privacy", "configuration", "static-addr", "phy-configuration",
                                      "wide-band-speech", "cis-central", "cis-peripheral", "iso-broadcaster",
                                      "iso-sync-receiver", "ll-privacy", "past-sender", "past-receiver", "sci"};
  std::string out;
  for (unsigned i = 0; i < 32; i++)
    if (v & (1u << i)) out += (out.empty() ? "" : " ") + std::string(i < sizeof kBits / sizeof *kBits ? kBits[i] : sf("bit%u", i));
  return out.empty() ? "none" : out;
}

std::string mgmt_label(const char* name, uint16_t code) { return name ? std::string(name) : sf("0x%04x", code); }

// An mgmt_addr_info (address, type) at p: the commands and events about one device start with it.
bool mgmt_has_addr(bool cmd, uint16_t code) {
  if (cmd) {
    switch (code) {
      case 0x0014: case 0x0016: case 0x0017: case 0x0019: case 0x001a: case 0x001b: case 0x001c:
      case 0x001d: case 0x001e: case 0x001f: case 0x0021: case 0x0022: case 0x0025: case 0x0026:
      case 0x0027: case 0x0031: case 0x0032: case 0x0033: case 0x0034: case 0x004f: case 0x0050:
        return true;
      default: return false;
    }
  }
  switch (code) {
    case 0x000b: case 0x000c: case 0x000d: case 0x000e: case 0x000f: case 0x0010: case 0x0011:
    case 0x0012: case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x001a: case 0x001b:
    case 0x001c: case 0x002a: case 0x0033: case 0x0034:
      return true;
    default: return false;
  }
}

std::string ad_data(Out& o, const uint8_t* p, size_t n);
std::string cod_str(uint32_t c);
const char* io_cap_name(uint8_t c);
const char* link_key_type_name(uint8_t t);
const char* bt_version_name(uint8_t v);
std::string company_str(uint16_t c);

// BlueZ's experimental features (src/adapter.c), which bluetoothd reads and sets at start.
const char* mgmt_exp_feature_name(const std::string& uuid) {
  if (uuid == "d4992530-b9ec-469f-ab01-6c481c47da1c") return "debug";
  if (uuid == "671b10b5-42c0-4696-9227-eb28d1b049d6") return "simultaneous central and peripheral";
  if (uuid == "330859bc-7506-492d-9370-9a6f0614037f") return "quality report";
  if (uuid == "15c0a148-c273-11ea-b3de-0242ac130004") return "LL privacy";
  if (uuid == "a6695ace-ee7f-4fb9-881a-5fac66c629af") return "codec offload";
  if (uuid == "6fbaf188-05e0-496a-9885-d6ddfdb4e03e") return "ISO socket";
  if (uuid == "2ce463d7-7a03-4d8d-bf05-5f24e8f36e76") return "mesh";
  if (uuid == "69518c4c-b69f-4679-8bc1-c021b47b5733") return "poll errqueue";
  return nullptr;
}

std::string mgmt_uuid_str(const uint8_t* p) {
  const std::string u = uuid_str(p, 16);
  // A 16-bit UUID in the Base UUID's form reads better as its name (Add UUID's service classes).
  static const uint8_t base[12] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
  if (std::memcmp(p, base, 12) == 0 && le16(p + 14) == 0) return uuid_str(p + 12, 2);
  const char* n = mgmt_exp_feature_name(u);
  return n ? u + " (" + n + ")" : u;
}

const char* const kMgmtExpFlags[] = {"active", "settings change"};
const char* const kMgmtDeviceFlags[] = {"remote wakeup", "device privacy mode", "address resolution", "past"};
const char* const kMgmtConnFlags[] = {"confirm name", "legacy pairing", "not connectable", "initiated pairing request",
                                      "name request failed", "scan response"};

// A management string field is NUL-padded to a fixed size: quoted up to the NUL.
std::string mgmt_text(const uint8_t* p, size_t n, size_t max) {
  const void* z = std::memchr(p, 0, n);
  return quoted(p, z ? static_cast<size_t>(static_cast<const uint8_t*>(z) - p) : n, max);
}

void mgmt_name(Rd& r, const char* name, size_t size) {
  if (!r.has(1)) return;
  const size_t k = std::min(size, r.left());
  if (r.o.detail) r.o.add(name, mgmt_text(r.cur(), k, 248), r.cur(), k);
  r.skip(k);
}

void mgmt_eir(Rd& r, size_t len) {
  Out& o = r.o;
  len = std::min(len, r.left());
  if (!len) return;
  OPEN("EIR", sf("%zu bytes", len), r.cur(), len);
  ad_data(o, r.cur(), len);
  CLOSE();
  r.skip(len);
}

// The parameters after the opcode (commands) or the event code (events), and the return
// parameters of a Command Complete (rsp): each command's layout from BlueZ's doc/mgmt-api.txt.
// Returns words for the summary.
std::string mgmt_params(Out& o, bool cmd, bool rsp, uint16_t code, const uint8_t* p, size_t n) {
  Rd r(o, p, n);
  std::string s;
  auto addr_info = [&]() {
    if (!r.has(7)) return;
    s += sf(" %s (%s)", bdaddr_str(r.cur()).c_str(), mgmt_addr_type(r.cur()[6]));
    r.addr();
    r.with("Address type", 1, [](uint32_t v) { return std::string(mgmt_addr_type(static_cast<uint8_t>(v))); });
  };
  auto settings = [&](const char* name) { r.with(name, 4, [](uint32_t v) { return mgmt_settings(v); }); };
  auto count_of = [&](const char* name, size_t each, const char* item) {
    const uint16_t k = r.u16(name);
    for (unsigned i = 0; i < k && r.has(each); ++i) r.hex(item, each);
  };
  if (rsp) {
    switch (code) {
      case 0x0001:  // Read Version
        if (n >= 3) {
          F("Version", sf("%u.%u", p[0], le16(p + 1)), p, 3);
          s = sf(" %u.%u", p[0], le16(p + 1));
        }
        return s;
      case 0x0002: {  // Read Commands
        const uint16_t nc = r.u16("Commands");
        const uint16_t ne = r.u16("Events");
        std::string cs, es;
        for (unsigned i = 0; i < nc && r.has(2); ++i, r.skip(2)) {
          const char* nm = mgmt_op_name(le16(r.cur()));
          if (o.detail) cs += (cs.empty() ? "" : ", ") + (nm ? std::string(nm) : sf("0x%04x", le16(r.cur())));
        }
        for (unsigned i = 0; i < ne && r.has(2); ++i, r.skip(2)) {
          const char* nm = mgmt_event_name(le16(r.cur()));
          if (o.detail) es += (es.empty() ? "" : ", ") + (nm ? std::string(nm) : sf("0x%04x", le16(r.cur())));
        }
        if (n >= 4) {
          F("Supported commands", cs, p + 4, std::min<size_t>(n - 4, nc * 2u));
          F("Supported events", es, p + 4 + nc * 2u, std::min<size_t>(n > 4 + nc * 2u ? n - 4 - nc * 2u : 0, ne * 2u));
          s = sf(" %u commands, %u events", nc, ne);
        }
        return s;
      }
      case 0x0003: {  // Read Index List
        const uint16_t k = r.u16("Controllers");
        for (unsigned i = 0; i < k && r.has(2); ++i) s += sf(" hci%u", r.u16("Index"));
        return s;
      }
      case 0x0004:  // Read Controller Info
        if (r.has(6)) s = " " + bdaddr_str(r.cur());
        r.addr();
        r.with("Version", 1, [](uint32_t v) { return sf("%u (Bluetooth %s)", v, bt_version_name(static_cast<uint8_t>(v))); });
        r.with("Manufacturer", 2, [](uint32_t v) { return company_str(static_cast<uint16_t>(v)); });
        settings("Supported settings");
        settings("Current settings");
        r.with("Class", 3, [](uint32_t v) { return cod_str(v); });
        if (r.has(1)) s += " " + mgmt_text(r.cur(), std::min<size_t>(249, r.left()), 40);
        mgmt_name(r, "Name", 249);
        mgmt_name(r, "Short name", 11);
        return s;
      case 0x000e:  // Set Dev Class
      case 0x0010:  // Add UUID
      case 0x0011:  // Remove UUID
        r.with("Class", 3, [](uint32_t v) { return cod_str(v); });
        return s;
      case 0x000f:  // Set Local Name
        mgmt_name(r, "Name", 249);
        mgmt_name(r, "Short name", 11);
        return s;
      case 0x0023:  // Start Discovery
      case 0x0024:  // Stop Discovery
      case 0x003a:  // Start Service Discovery
      case 0x0041:  // Start Limited Discovery
        r.u8("Address types", "0x%02x");
        return s;
      case 0x003d: {  // Read Advertising Features
        r.u32("Supported flags", "0x%08x");
        r.u8("Max advertising data length");
        r.u8("Max scan response length");
        r.u8("Max instances");
        const uint8_t k = r.u8("Instances");
        for (unsigned i = 0; i < k && r.has(1); ++i) r.u8("Instance");
        return s;
      }
      case 0x003e:  // Add Advertising
      case 0x003f:  // Remove Advertising
      case 0x0054:  // Add Extended Advertising Parameters
      case 0x0055:  // Add Extended Advertising Data
        s = sf(" instance %u", r.u8("Instance"));
        if (code == 0x0054) {
          r.dbm("TX power");
          r.u8("Max advertising data length");
          r.u8("Max scan response length");
        }
        return s;
      case 0x0048: {  // Read Controller Capabilities Information
        const uint16_t l = r.u16("Capabilities length");
        Rd c(o, r.cur(), std::min<size_t>(l, r.left()));
        while (c.has(2)) {
          const uint8_t el = c.cur()[0], t = c.cur()[1];
          if (!el || !c.has(1 + el)) break;
          const uint8_t* v = c.cur() + 2;
          std::string name = sf("Capability 0x%02x", t), val = hexbytes(v, el - 1);
          if (t == 0x01 && el >= 2) {
            static const char* const fl[] = {"remote public key validation", "encryption key size enforcement"};
            name = "Security flags";
            val = sf("0x%02x (%s)", v[0], bit_names(v[0], fl, 2).c_str());
          } else if ((t == 0x02 || t == 0x03) && el >= 2) {
            name = t == 0x02 ? "Max encryption key size (BR/EDR)" : "Max encryption key size (LE)";
            val = std::to_string(v[0]);
          } else if (t == 0x04 && el >= 3) {
            name = "LE TX power";
            val = sf("%d to %d dBm", static_cast<int8_t>(v[0]), static_cast<int8_t>(v[1]));
          }
          F(name, val, c.cur(), 1 + el);
          c.skip(1 + el);
        }
        r.skip(l);
        return s;
      }
      case 0x0049: {  // Read Experimental Features Information
        const uint16_t k = r.u16("Features");
        for (unsigned i = 0; i < k && r.has(20); ++i) {
          const uint8_t* e = r.cur();
          const uint32_t fl = le32(e + 16);
          if (o.detail) {
            const std::string u = uuid_str(e, 16);
            const char* nm = mgmt_exp_feature_name(u);
            o.add(nm ? nm : "Feature", u + sf(", flags 0x%08x (", fl) + bit_names(fl, kMgmtExpFlags, 2) + ")", e, 20);
          }
          if (fl & 1) {
            const char* nm = mgmt_exp_feature_name(uuid_str(e, 16));
            s += std::string(s.empty() ? " active: " : ", ") + (nm ? nm : "?");
          }
          r.skip(20);
        }
        return s;
      }
      case 0x004a:  // Set Experimental Feature
        r.with("UUID", 16, [&](uint32_t) { return mgmt_uuid_str(r.cur()); });
        r.flags("Flags", 4, kMgmtExpFlags, 2);
        return s;
      case 0x0051: {  // Read Advertisement Monitor Features
        r.u32("Supported features", "0x%08x");
        r.u32("Enabled features", "0x%08x");
        r.u16("Max handles");
        r.u8("Max patterns");
        const uint16_t k = r.u16("Handles");
        for (unsigned i = 0; i < k && r.has(2); ++i) r.u16("Handle");
        return s;
      }
      case 0x0052:  // Add Advertisement Patterns Monitor
      case 0x0053:  // Remove Advertisement Monitor
      case 0x0056:
        s = sf(" monitor %u", r.u16("Monitor handle"));
        return s;
      default:
        break;
    }
    // The commands about one device answer with its address.
    if (mgmt_has_addr(true, code)) {
      addr_info();
      if (code == 0x0031) {  // Get Connection Information
        if (r.has(1)) s += sf(" RSSI %d", static_cast<int8_t>(r.cur()[0]));
        r.with("RSSI", 1, [](uint32_t v) { return static_cast<int8_t>(v) == 127 ? std::string("unknown") : sf("%d dBm", static_cast<int8_t>(v)); });
        r.with("TX power", 1, [](uint32_t v) { return static_cast<int8_t>(v) == 127 ? std::string("unknown") : sf("%d dBm", static_cast<int8_t>(v)); });
        r.with("Max TX power", 1, [](uint32_t v) { return static_cast<int8_t>(v) == 127 ? std::string("unknown") : sf("%d dBm", static_cast<int8_t>(v)); });
      } else if (code == 0x0032) {  // Get Clock Information
        r.u32("Local clock", "0x%08x");
        r.u32("Piconet clock", "0x%08x");
        r.u16("Accuracy");
      } else if (code == 0x004f) {  // Get Device Flags
        r.flags("Supported flags", 4, kMgmtDeviceFlags, 4);
        r.flags("Current flags", 4, kMgmtDeviceFlags, 4);
      }
    }
    return s;
  }

  if (cmd) {
    switch (code) {
      case 0x000e:  // Set Dev Class
        r.u8("Major class", "0x%02x");
        r.u8("Minor class", "0x%02x");
        return s;
      case 0x000f:  // Set Local Name
        if (r.has(1)) s = " " + mgmt_text(r.cur(), std::min<size_t>(249, r.left()), 40);
        mgmt_name(r, "Name", 249);
        mgmt_name(r, "Short name", 11);
        return s;
      case 0x0010:  // Add UUID
      case 0x0011:  // Remove UUID
        if (r.has(16)) s = " " + mgmt_uuid_str(r.cur());
        r.with("UUID", 16, [&](uint32_t) { return mgmt_uuid_str(r.cur()); });
        if (code == 0x0010) r.with("Service class hint", 1, [](uint32_t v) { return cod_str(v << 16); });
        if (code == 0x0011 && n >= 16 && std::all_of(p, p + 16, [](uint8_t b) { return b == 0; })) s = " all";
        return s;
      case 0x0012:  // Load Link Keys
        r.yes_no("Debug keys", "allowed", "not allowed");
        s = sf(" %u keys", r.raw(2));
        count_of("Keys", 25, "Link key");
        return s;
      case 0x0013:  // Load Long Term Keys
        s = sf(" %u keys", r.raw(2));
        count_of("Keys", 36, "Long term key");
        return s;
      case 0x0030:  // Load Identity Resolving Keys
        s = sf(" %u keys", r.raw(2));
        count_of("Keys", 23, "Identity resolving key");
        return s;
      case 0x0035:  // Load Connection Parameters
        s = sf(" %u devices", r.raw(2));
        count_of("Parameters", 15, "Connection parameters");
        return s;
      case 0x0046:  // Set Blocked Keys
        s = sf(" %u keys", r.raw(2));
        count_of("Keys", 17, "Blocked key");
        return s;
      case 0x0018:  // Set IO Capability
        if (n >= 1) s = std::string(" ") + io_cap_name(p[0]);
        r.with("IO capability", 1, [](uint32_t v) { return std::string(io_cap_name(static_cast<uint8_t>(v))); });
        return s;
      case 0x0023:  // Start Discovery
      case 0x0024:  // Stop Discovery
      case 0x003a:  // Start Service Discovery
      case 0x0041:  // Start Limited Discovery
        r.with("Address types", 1, [](uint32_t v) {
          static const char* const t[] = {"BR/EDR", "LE public", "LE random"};
          return sf("0x%02x (%s)", v, bit_names(v, t, 3).c_str());
        });
        if (code == 0x003a) {
          r.with("RSSI threshold", 1, [](uint32_t v) { return sf("%d dBm", static_cast<int8_t>(v)); });
          const uint16_t k = r.u16("UUIDs");
          for (unsigned i = 0; i < k && r.has(16); ++i) r.with("UUID", 16, [&](uint32_t) { return mgmt_uuid_str(r.cur()); });
        }
        return s;
      case 0x0028:  // Set Device ID
        r.with("Source", 2, [](uint32_t v) { return std::string(v == 1 ? "Bluetooth SIG" : v == 2 ? "USB" : v ? "?" : "disabled"); });
        r.u16("Vendor", "0x%04x");
        r.u16("Product", "0x%04x");
        r.u16("Version", "0x%04x");
        return s;
      case 0x002f:  // Set Privacy
        if (n >= 1) s = p[0] ? (p[0] == 2 ? " limited" : " on") : " off";
        r.with("Privacy", 1, [](uint32_t v) { return std::string(v == 0 ? "off" : v == 1 ? "on" : v == 2 ? "limited" : "?"); });
        r.hex("Identity resolving key", 16);
        return s;
      case 0x003e: {  // Add Advertising
        s = sf(" instance %u", r.u8("Instance"));
        r.u32("Flags", "0x%08x");
        r.u16("Duration", "%u s");
        r.u16("Timeout", "%u s");
        const uint8_t al = r.u8("Advertising data length");
        const uint8_t sl = r.u8("Scan response length");
        if (al && r.has(1)) {
          OPEN("Advertising data", "", r.cur(), std::min<size_t>(al, r.left()));
          ad_data(o, r.cur(), std::min<size_t>(al, r.left()));
          CLOSE();
          r.skip(al);
        }
        if (sl && r.has(1)) {
          OPEN("Scan response", "", r.cur(), std::min<size_t>(sl, r.left()));
          ad_data(o, r.cur(), std::min<size_t>(sl, r.left()));
          CLOSE();
          r.skip(sl);
        }
        return s;
      }
      case 0x003f:  // Remove Advertising
        s = sf(" instance %u", r.u8("Instance"));
        return s;
      case 0x0047:  // Set Wideband Speech
        if (n >= 1) s = p[0] ? " on" : " off";
        r.yes_no("Mode", "on", "off");
        return s;
      case 0x004a:  // Set Experimental Feature
        if (r.has(16)) s = " " + mgmt_uuid_str(r.cur());
        r.with("UUID", 16, [&](uint32_t) { return mgmt_uuid_str(r.cur()); });
        if (r.has(1)) s += r.cur()[0] ? " on" : " off";
        r.yes_no("Action", "on", "off");
        return s;
      case 0x0053:  // Remove Advertisement Monitor
        s = sf(" monitor %u", r.u16("Monitor handle", "%u (0: all)"));
        return s;
      default:
        break;
    }
    if (!mgmt_has_addr(true, code)) return s;
    addr_info();
    switch (code) {
      case 0x0019:  // Pair Device
        r.with("IO capability", 1, [](uint32_t v) { return std::string(io_cap_name(static_cast<uint8_t>(v))); });
        break;
      case 0x001b:  // Unpair Device
        r.yes_no("Disconnect", "yes", "no");
        break;
      case 0x0016:  // PIN Code Reply
        r.u8("PIN length");
        r.hex("PIN", 16);
        break;
      case 0x001e:  // User Passkey Reply
        r.u32("Passkey", "%06u");
        break;
      case 0x0033: {  // Add Device
        static const char* const act[] = {"background scan", "allow incoming connection", "auto-connect"};
        if (r.has(1)) s += sf(", %s", r.cur()[0] < 3 ? act[r.cur()[0]] : "?");
        r.with("Action", 1, [](uint32_t v) { return sf("0x%02x (%s)", v, v < 3 ? act[v] : "?"); });
        break;
      }
      case 0x0050:  // Set Device Flags
        r.flags("Current flags", 4, kMgmtDeviceFlags, 4);
        break;
      default:
        break;
    }
    return s;
  }

  // Events.
  switch (code) {
    case 0x0007:  // Class of Device Changed
      if (n >= 3) s = " " + cod_str(le24(p));
      r.with("Class", 3, [](uint32_t v) { return cod_str(v); });
      return s;
    case 0x0008:  // Local Name Changed
      if (r.has(1)) s = " " + mgmt_text(r.cur(), std::min<size_t>(249, r.left()), 40);
      mgmt_name(r, "Name", 249);
      mgmt_name(r, "Short name", 11);
      return s;
    case 0x0009:  // New Link Key
    case 0x000a:  // New Long Term Key
    case 0x0018:  // New Identity Resolving Key
    case 0x0019:  // New Signature Resolving Key
      r.yes_no("Store hint", "store", "do not store");
      if (code == 0x0018) r.addr("Random address");
      addr_info();
      if (code == 0x0009) {
        r.with("Key type", 1, [](uint32_t v) { return sf("0x%02x (%s)", v, link_key_type_name(static_cast<uint8_t>(v))); });
        r.hex("Key", 16);
        r.u8("PIN length");
      } else if (code == 0x000a) {
        r.u8("Key type", "0x%02x");
        r.yes_no("Central", "yes", "no");
        r.u8("Encryption key size");
        r.u16("EDIV", "0x%04x");
        r.hex("Rand", 8);
        r.hex("Key", 16);
      } else if (code == 0x0018) {
        r.hex("Key", 16);
      } else {
        r.u8("Key type", "0x%02x");
        r.hex("Key", 16);
      }
      return s;
    case 0x0023:  // Advertising Added
    case 0x0024:  // Advertising Removed
      s = sf(" instance %u", r.u8("Instance"));
      return s;
    case 0x0027:  // Experimental Feature Changed
      if (r.has(16)) s = " " + mgmt_uuid_str(r.cur());
      r.with("UUID", 16, [&](uint32_t) { return mgmt_uuid_str(r.cur()); });
      if (r.has(4)) s += r.cur()[0] & 1 ? " active" : " inactive";
      r.flags("Flags", 4, kMgmtExpFlags, 2);
      return s;
    case 0x0028:  // Default System Configuration Changed
    case 0x0029:  // Default Runtime Configuration Changed
      return s;
    case 0x002b:  // Advertisement Monitor Added
    case 0x002c:  // Advertisement Monitor Removed
      s = sf(" monitor %u", r.u16("Monitor handle"));
      return s;
    case 0x002f:  // Advertisement Monitor Device Found
    case 0x0030:  // Advertisement Monitor Device Lost
      s = sf(" monitor %u", r.u16("Monitor handle"));
      addr_info();
      if (code == 0x002f) {
        r.with("RSSI", 1, [](uint32_t v) { return sf("%d dBm", static_cast<int8_t>(v)); });
        r.flags("Flags", 4, kMgmtConnFlags, 6);
        mgmt_eir(r, r.u16("EIR length"));
      }
      return s;
    case 0x002d:  // Controller Suspend
      r.with("Suspend state", 1, [](uint32_t v) {
        static const char* const st[] = {"running", "disconnected, not scanning", "page scan / passive scan"};
        return std::string(v < 3 ? st[v] : "?");
      });
      return s;
    default:
      break;
  }
  const bool addr_first = mgmt_has_addr(false, code) || code == 0x002a || code == 0x0033 || code == 0x0034;
  if (!addr_first) return s;
  addr_info();
  switch (code) {
    case 0x000b:  // Device Connected
      r.flags("Flags", 4, kMgmtConnFlags, 6);
      mgmt_eir(r, r.u16("EIR length"));
      break;
    case 0x000c:  // Device Disconnected
      if (r.has(1)) {
        static const char* const kWhy[] = {"unknown", "timeout", "local host", "remote", "auth failure", "local host suspend"};
        const char* why = r.cur()[0] < 6 ? kWhy[r.cur()[0]] : "?";
        s += sf(", reason %s", why);
        r.with("Reason", 1, [&](uint32_t v) { return sf("0x%02x (%s)", v, why); });
      }
      break;
    case 0x000d:  // Connect Failed
    case 0x0011:  // Authentication Failed
      if (r.has(1)) {
        const char* st = mgmt_status_name(r.cur()[0]);
        s += sf(", %s", st ? st : "failed");
        r.with("Status", 1, [&](uint32_t v) { return sf("0x%02x (%s)", v, st ? st : "unknown"); });
      }
      break;
    case 0x000f:  // User Confirm Request
      r.yes_no("Confirm hint", "just works", "compare");
      r.u32("Value", "%06u");
      break;
    case 0x0012: {  // Device Found
      if (r.has(1)) s += sf(" %d dBm", static_cast<int8_t>(r.cur()[0]));
      r.with("RSSI", 1, [](uint32_t v) { return sf("%d dBm", static_cast<int8_t>(v)); });
      r.flags("Flags", 4, kMgmtConnFlags, 6);
      mgmt_eir(r, r.u16("EIR length"));
      break;
    }
    case 0x0017:  // Passkey Notify
      r.u32("Passkey", "%06u");
      r.u8("Entered");
      break;
    case 0x001a: {  // Device Added
      static const char* const act[] = {"background scan", "allow incoming connection", "auto-connect"};
      r.with("Action", 1, [](uint32_t v) { return sf("0x%02x (%s)", v, v < 3 ? act[v] : "?"); });
      break;
    }
    case 0x001c:  // New Connection Parameter
      r.yes_no("Store hint", "store", "do not store");
      r.time("Min interval", 2, 1.25);
      r.time("Max interval", 2, 1.25);
      r.u16("Latency");
      r.time("Supervision timeout", 2, 10);
      break;
    case 0x002a:  // Device Flags Changed
      r.flags("Supported flags", 4, kMgmtDeviceFlags, 4);
      r.flags("Current flags", 4, kMgmtDeviceFlags, 4);
      break;
    case 0x0033:  // Connection Subrate
      r.status();
      r.time("Interval", 2, 1.25);
      r.u16("Subrate factor");
      r.u16("Latency");
      r.u16("Continuation number");
      r.time("Supervision timeout", 2, 10);
      break;
    case 0x0034: {  // Security Level Changed: TLVs of { type (2), length (1), value }
      const uint8_t l = r.u8("TLV length");
      Rd t(o, r.cur(), std::min<size_t>(l, r.left()));
      while (t.has(3)) {
        const uint16_t type = le16(t.cur());
        const uint8_t vl = t.cur()[2];
        if (!t.has(3 + vl)) break;
        const uint8_t* v = t.cur() + 3;
        if (type == 0x0000 && vl >= 1) {
          F("Security level", std::to_string(v[0]), t.cur(), 3 + vl);
          s += sf(", level %u", v[0]);
        } else if (type == 0x0001 && vl >= 1) {
          static const char* const enc[] = {"none", "E0", "AES-CCM"};
          F("Encryption", v[0] < 3 ? enc[v[0]] : "?", t.cur(), 3 + vl);
        } else {
          F(sf("TLV 0x%04x", type), hexbytes(v, vl), t.cur(), 3 + vl);
        }
        t.skip(3 + vl);
      }
      r.skip(l);
      break;
    }
    default:
      break;
  }
  return s;
}

void dissect_mgmt(Out& o, bool cmd, const uint8_t* d, size_t len) {
  const uint16_t code = le16(d + 4);
  const uint8_t* p = d + 6;
  size_t n = len - 6;
  F("Cookie", sf("0x%08x", le32(d)), d, 4);
  // Command Complete / Status answer an opcode: the summary names the command, then the outcome.
  if (!cmd && (code == 0x0001 || code == 0x0002) && n >= 3) {
    const uint16_t op = le16(p);
    const char* st = mgmt_status_name(p[2]);
    F("Event", mgmt_label(mgmt_event_name(code), code), d + 4, 2);
    F("Command", sf("%s (0x%04x)", mgmt_label(mgmt_op_name(op), op).c_str(), op), p, 2);
    F("Status", sf("0x%02x (%s)", p[2], st ? st : "unknown"), p + 2, 1);
    o.summary = sf("MGMT %s: %s%s", code == 1 ? "Command Complete" : "Command Status", mgmt_label(mgmt_op_name(op), op).c_str(),
                   p[2] ? sf(" — %s", st ? st : sf("status 0x%02x", p[2]).c_str()).c_str() : "");
    p += 3;
    n -= 3;
    o.mark(p);
    // The Set commands that change a setting answer with the settings in force after it.
    if (code == 0x0001 && !p[-1] && n == 4 && ((op >= 0x0005 && op <= 0x000d) || op == 0x0029 || op == 0x002a ||
                                               op == 0x002d || op == 0x002e || op == 0x002f || op == 0x0047)) {
      F("Current settings", mgmt_settings(le32(p)), p, 4);
      o.summary += " → " + mgmt_settings(le32(p));
      return;
    }
    if (code == 0x0001 && n) o.summary += mgmt_params(o, false, true, op, p, n);
    rest(o, p, n, "Return parameters");
    return;
  }
  const std::string name = mgmt_label(cmd ? mgmt_op_name(code) : mgmt_event_name(code), code);
  F(cmd ? "Opcode" : "Event", sf("%s (0x%04x)", name.c_str(), code), d + 4, 2);
  o.summary = std::string(cmd ? "MGMT " : "MGMT Event: ") + name;
  o.mark(p);
  if (cmd && ((code >= 0x0005 && code <= 0x000d) || code == 0x0029 || code == 0x002a || code == 0x002d) && n >= 1) {
    // Set Powered … Set LE, Set Advertising, Set BR/EDR, Set Secure Connections: a mode byte.
    const char* mode = p[0] == 0 ? "off" : p[0] == 1 ? "on" : p[0] == 2 ? (code == 0x0006 ? "limited" : "only") : "?";
    F("Mode", sf("0x%02x (%s)", p[0], mode), p, 1);
    o.summary += sf(" %s", mode);
    if (code == 0x0006 && n >= 3) {
      F("Timeout", sf("%u s", le16(p + 1)), p + 1, 2);
      if (le16(p + 1)) o.summary += sf(" for %u s", le16(p + 1));
    }
    return;
  } else if (!cmd && code == 0x0006 && n >= 4) {
    F("Current settings", mgmt_settings(le32(p)), p, 4);
    o.summary += " " + mgmt_settings(le32(p));
    return;
  } else if (!cmd && code == 0x0013 && n >= 2) {
    F("Address type", sf("0x%02x", p[0]), p, 1);
    F("Discovering", p[1] ? "yes" : "no", p + 1, 1);
    o.summary += p[1] ? " started" : " stopped";
    return;
  }
  o.summary += mgmt_params(o, cmd, false, code, p, n);
  rest(o, p, n, "Data");
}

const char* sig_name(uint8_t code) {
  switch (code) {
    case 0x01: return "Command Reject";
    case 0x02: return "Connection Request";
    case 0x03: return "Connection Response";
    case 0x04: return "Configuration Request";
    case 0x05: return "Configuration Response";
    case 0x06: return "Disconnection Request";
    case 0x07: return "Disconnection Response";
    case 0x08: return "Echo Request";
    case 0x09: return "Echo Response";
    case 0x0a: return "Information Request";
    case 0x0b: return "Information Response";
    case 0x0c: return "Create Channel Request";
    case 0x0d: return "Create Channel Response";
    case 0x0e: return "Move Channel Request";
    case 0x0f: return "Move Channel Response";
    case 0x10: return "Move Channel Confirmation";
    case 0x11: return "Move Channel Confirmation Response";
    case 0x12: return "Connection Parameter Update Request";
    case 0x13: return "Connection Parameter Update Response";
    case 0x14: return "LE Credit Based Connection Request";
    case 0x15: return "LE Credit Based Connection Response";
    case 0x16: return "Flow Control Credit";
    case 0x17: return "Credit Based Connection Request";
    case 0x18: return "Credit Based Connection Response";
    case 0x19: return "Credit Based Reconfigure Request";
    case 0x1a: return "Credit Based Reconfigure Response";
    default: return "Unknown";
  }
}

const char* conn_result_name(uint16_t r) {
  switch (r) {
    case 0x0000: return "success";
    case 0x0001: return "pending";
    case 0x0002: return "PSM not supported";
    case 0x0003: return "security block";
    case 0x0004: return "no resources";
    case 0x0005: return "invalid parameters";  // LE: SPSM not supported
    case 0x0006: return "invalid source CID";
    case 0x0007: return "source CID already allocated";
    case 0x0008: return "insufficient encryption key size";
    case 0x0009: return "invalid parameters";
    default: return "refused";
  }
}

std::string psm_str(uint16_t psm) {
  switch (psm) {
    case kPsmSdp: return "0x0001 (SDP)";
    case kPsmRfcomm: return "0x0003 (RFCOMM)";
    case kPsmBnep: return "0x000f (BNEP)";
    case kPsmHidCtrl: return "0x0011 (HID control)";
    case kPsmHidIntr: return "0x0013 (HID interrupt)";
    case kPsmAvctp: return "0x0017 (AVCTP)";
    case kPsmAvdtp: return "0x0019 (AVDTP)";
    case kPsmAvctpBrowsing: return "0x001b (AVCTP browsing)";
    case kPsmAtt: return "0x001f (ATT)";
    case kPsmEatt: return "0x0027 (EATT)";
    default: return sf("0x%04x", psm);
  }
}

std::string cid_str(uint16_t cid) {
  switch (cid) {
    case kCidSignaling: return "0x0001 (L2CAP signalling)";
    case kCidConnectionless: return "0x0002 (connectionless)";
    case kCidAtt: return "0x0004 (ATT)";
    case kCidLeSignaling: return "0x0005 (LE signalling)";
    case kCidSmp: return "0x0006 (SMP)";
    case kCidSmpBredr: return "0x0007 (SMP BR/EDR)";
    default: return sf("0x%04x", cid);
  }
}

// ---------------------------------------------------------------------------------------------
// HCI commands

// Class of Device (Assigned Numbers 2.8): the major device class and its service classes.
std::string cod_str(uint32_t c) {
  static const char* major[] = {"miscellaneous", "computer", "phone", "LAN/network", "audio/video", "peripheral",
                                "imaging", "wearable", "toy", "health"};
  static const char* service[] = {"limited discoverable", "LE audio", "reserved", "positioning", "networking",
                                  "rendering", "capturing", "object transfer", "audio", "telephony", "information"};
  const unsigned mj = (c >> 8) & 0x1f;
  std::string s = sf("0x%06x %s", c, mj < 10 ? major[mj] : mj == 0x1f ? "uncategorized" : "?");
  std::string sv;
  for (int b = 13; b <= 23; ++b)
    if (c & (1u << b)) sv += (sv.empty() ? "" : ", ") + std::string(service[b - 13]);
  if (!sv.empty()) s += " (" + sv + ")";
  return s;
}

// LMP feature bits (Core Vol 2 Part C 3.3), per page of Read Local/Remote (Extended) Features.
const char* const kLmpFeatures0[64] = {
    "3-slot packets", "5-slot packets", "encryption", "slot offset", "timing accuracy", "role switch",
    "hold mode", "sniff mode", "previously used", "power control requests", "CQDDR", "SCO link",
    "HV2 packets", "HV3 packets", "u-law log", "A-law log", "CVSD", "paging parameter negotiation",
    "power control", "transparent SCO data", "flow control lag (lsb)", "flow control lag (middle)",
    "flow control lag (msb)", "broadcast encryption", nullptr, "EDR ACL 2 Mb/s", "EDR ACL 3 Mb/s",
    "enhanced inquiry scan", "interlaced inquiry scan", "interlaced page scan", "RSSI with inquiry results",
    "eSCO EV3", "EV4 packets", "EV5 packets", nullptr, "AFH capable peripheral", "AFH classification peripheral",
    "BR/EDR not supported", "LE supported (controller)", "3-slot EDR ACL", "5-slot EDR ACL", "sniff subrating",
    "pause encryption", "AFH capable central", "AFH classification central", "EDR eSCO 2 Mb/s",
    "EDR eSCO 3 Mb/s", "3-slot EDR eSCO", "extended inquiry response", "simultaneous LE and BR/EDR (controller)",
    nullptr, "secure simple pairing (controller)", "encapsulated PDU", "erroneous data reporting",
    "non-flushable packet boundary flag", nullptr, "link supervision timeout changed event",
    "variable inquiry TX power level", "enhanced power control", nullptr, nullptr, nullptr, nullptr,
    "extended features"};
const char* const kLmpFeatures1[] = {"secure simple pairing (host)", "LE supported (host)",
                                     "simultaneous LE and BR/EDR (host)", "secure connections (host)"};
const char* const kLmpFeatures2[] = {
    "connectionless peripheral broadcast central", "connectionless peripheral broadcast peripheral",
    "synchronization train", "synchronization scan", "inquiry response notification event",
    "generalized interlaced scan", "coarse clock adjustment", nullptr, "secure connections (controller)",
    "ping", "slot availability mask", "train nudging"};

std::string lmp_features_str(const uint8_t* p, unsigned page) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
  if (page == 0) return bit_names(v, kLmpFeatures0, 64);
  if (page == 1) return bit_names(v, kLmpFeatures1, 4);
  if (page == 2) return bit_names(v, kLmpFeatures2, 12);
  return hexbytes(p, 8);
}

// LE link layer feature bits (Core Vol 6 Part B 4.6), which also number LE Set Host Feature's bit.
const char* const kLeFeatures[] = {
    "LE encryption", "connection parameters request", "extended reject indication",
    "peripheral-initiated features exchange", "LE ping", "data packet length extension", "LL privacy",
    "extended scanner filter policies", "LE 2M PHY", "stable modulation index TX", "stable modulation index RX",
    "LE Coded PHY", "extended advertising", "periodic advertising", "channel selection algorithm #2",
    "power class 1", "minimum number of used channels", "connection CTE request", "connection CTE response",
    "connectionless CTE TX", "connectionless CTE RX", "AoD", "AoA", "receiving CTE",
    "periodic advertising sync transfer sender", "periodic advertising sync transfer recipient",
    "sleep clock accuracy updates", "remote public key validation", "CIS central", "CIS peripheral",
    "isochronous broadcaster", "synchronized receiver", "CIS (host support)", "power control request",
    "power control request (2)", "path loss monitoring", "periodic advertising ADI", "connection subrating",
    "connection subrating (host support)", "channel classification", "advertising coding selection",
    "advertising coding selection (host support)", "decision-based advertising filtering",
    "periodic advertising with responses advertiser", "periodic advertising with responses scanner",
    "unsegmented framed mode", "channel sounding", "channel sounding (host support)",
    "channel sounding tone quality indication"};
constexpr size_t kLeFeatureCount = sizeof kLeFeatures / sizeof *kLeFeatures;

std::string le_features_str(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
  return bit_names(v, kLeFeatures, kLeFeatureCount);
}

// The Bluetooth version an HCI or LMP version number stands for (Assigned Numbers 2.1).
const char* bt_version_name(uint8_t v) {
  static const char* const names[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1",
                                      "4.2",  "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"};
  return v < sizeof names / sizeof *names ? names[v] : "?";
}

std::string company_str(uint16_t c) {
  const char* n = company_name(c);
  return n ? sf("%u (%s)", c, n) : std::to_string(c);
}

// Coding formats (Assigned Numbers 2.11): HCI codec IDs, LE Audio's codec IDs.
const char* coding_format_name(uint8_t f) {
  switch (f) {
    case 0x00: return "u-law log";
    case 0x01: return "A-law log";
    case 0x02: return "CVSD";
    case 0x03: return "transparent";
    case 0x04: return "linear PCM";
    case 0x05: return "mSBC";
    case 0x06: return "LC3";
    case 0x07: return "G.729A";
    case 0x08: return "LC3plus";
    case 0xff: return "vendor";
    default: return "?";
  }
}

// A codec ID (coding format, company, vendor codec ID): "LC3 (0x06)", "vendor 0x0075:0x0001 (Samsung)".
std::string codec_id_str(const uint8_t* p) {
  if (p[0] != 0xff) return sf("%s (0x%02x)", coding_format_name(p[0]), p[0]);
  const char* c = company_name(le16(p + 1));
  return sf("vendor 0x%04x:0x%04x", le16(p + 1), le16(p + 3)) + (c ? std::string(" (") + c + ")" : "");
}

const char* const kAdvProps[] = {"connectable", "scannable", "directed", "high duty cycle directed",
                                 "legacy", "anonymous", "include TX power"};
const char* const kAdvChannels[] = {"37", "38", "39"};
const char* const kLinkPolicy[] = {"role switch", "hold mode", "sniff mode", "park state"};
// ACL packet types: the 2-DH and 3-DH bits say "may not be used".
const char* const kAclPacketTypes[] = {nullptr, "no 2-DH1", "no 3-DH1", "DM1", "DH1", nullptr, nullptr, nullptr,
                                       "no 2-DH3", "no 3-DH3", "DM3", "DH3", "no 2-DH5", "no 3-DH5", "DM5", "DH5"};
const char* const kSyncPacketTypes[] = {"HV1", "HV2", "HV3", "EV3", "EV4", "EV5",
                                        "no 2-EV3", "no 3-EV3", "no 2-EV5", "no 3-EV5"};

const char* adv_filter_name(uint32_t f) {
  switch (f) {
    case 0: return "any scan and connect request";
    case 1: return "scan requests from the accept list";
    case 2: return "connect requests from the accept list";
    case 3: return "both from the accept list";
    default: return "?";
  }
}
const char* scan_filter_name(uint32_t f) {
  switch (f) {
    case 0: return "accept all";
    case 1: return "accept list only";
    case 2: return "accept all, directed to an RPA too";
    case 3: return "accept list, directed to an RPA too";
    default: return "?";
  }
}
const char* adv_type_name(uint32_t t) {
  switch (t) {
    case 0: return "ADV_IND";
    case 1: return "ADV_DIRECT_IND (high duty cycle)";
    case 2: return "ADV_SCAN_IND";
    case 3: return "ADV_NONCONN_IND";
    case 4: return "ADV_DIRECT_IND (low duty cycle)";
    default: return "?";
  }
}
const char* adv_data_op_name(uint32_t op) {
  switch (op) {
    case 0: return "intermediate fragment";
    case 1: return "first fragment";
    case 2: return "last fragment";
    case 3: return "complete data";
    case 4: return "unchanged data (new DID)";
    default: return "?";
  }
}
// Sleep clock accuracy classes (Core Vol 4 Part E 7.7.65.25).
const char* sca_name(uint32_t s) {
  static const char* const names[] = {"251-500 ppm", "151-250 ppm", "101-150 ppm", "76-100 ppm",
                                      "51-75 ppm",   "31-50 ppm",   "21-30 ppm",   "0-20 ppm"};
  return s < 8 ? names[s] : "?";
}
const char* retrans_effort_name(uint32_t r) {
  switch (r) {
    case 0x00: return "no retransmissions";
    case 0x01: return "optimize for power";
    case 0x02: return "optimize for link quality";
    case 0xff: return "don't care";
    default: return "?";
  }
}
const char* auth_req_name(uint32_t a) {
  switch (a) {
    case 0: return "no bonding";
    case 1: return "no bonding, MITM";
    case 2: return "dedicated bonding";
    case 3: return "dedicated bonding, MITM";
    case 4: return "general bonding";
    case 5: return "general bonding, MITM";
    default: return "?";
  }
}
const char* oob_present_name(uint32_t v) {
  switch (v) {
    case 0: return "not present";
    case 1: return "P-192";
    case 2: return "P-256";
    case 3: return "P-192 and P-256";
    default: return "?";
  }
}
const char* inquiry_mode_name(uint32_t m) {
  switch (m) {
    case 0: return "standard";
    case 1: return "with RSSI";
    case 2: return "with RSSI or extended";
    default: return "?";
  }
}
// The voice setting (Core Vol 4 Part E 6.12): the air coding and the host side's sample format.
std::string voice_setting_str(uint32_t v) {
  static const char* const air[] = {"CVSD", "u-law", "A-law", "transparent"};
  static const char* const in[] = {"linear", "u-law", "A-law", "?"};
  return sf("0x%04x (air coding %s, input %s %u-bit)", v, air[v & 3], in[(v >> 8) & 3], v & 0x20 ? 16 : 8);
}
const char* data_path_name(uint32_t id) { return id == 0 ? "HCI" : id == 0xff ? "disabled" : "vendor"; }
const char* privacy_mode_name(uint32_t m) { return m == 0 ? "network" : m == 1 ? "device" : "?"; }
const char* past_mode_name(uint32_t m) {
  switch (m) {
    case 0: return "no sync";
    case 1: return "sync, reports disabled";
    case 2: return "sync, reports enabled";
    case 3: return "sync, reports with duplicate filtering";
    default: return "?";
  }
}
const char* const kPaSyncOptions[] = {"use periodic advertiser list", "reporting initially disabled",
                                      "duplicate filtering initially enabled"};
const char* const kPaReceiveEnable[] = {"reporting", "duplicate filtering"};
const char* const kAddrChangeReasons[] = {"advertising data", "scan response data"};
const char* const kCteTypes[] = {"no AoA CTE", "no AoD CTE 1 us", "no AoD CTE 2 us", "no type 3 CTE", "only with CTE"};

const char* io_cap_name(uint8_t c);

// ---------------------------------------------------------------------------------------------
// LE Audio: the LTV structures of BAP (codec configuration and capabilities, metadata), audio
// contexts and locations (Assigned Numbers 6.12, 6.4, 6.12.1). Advertising (BASE, announcements)
// and ATT values (ASE states, PAC records, BASS) carry them.

const char* const kAudioContexts[] = {"unspecified", "conversational", "media", "game", "instructional",
                                      "voice assistants", "live", "sound effects", "notifications", "ringtone",
                                      "alerts", "emergency alarm"};
std::string contexts_str(uint16_t v) { return sf("0x%04x (%s)", v, bit_names(v, kAudioContexts, 12).c_str()); }

const char* const kAudioLocations[] = {"FL",  "FR",  "FC",  "LFE1", "BL",  "BR",  "FLC", "FRC", "BC",  "LFE2",
                                       "SL",  "SR",  "TFL", "TFR",  "TFC", "TC",  "TBL", "TBR", "TSL", "TSR",
                                       "TBC", "BFC", "BFL", "BFR",  "FLW", "FRW", "LS",  "RS"};
std::string locations_str(uint32_t v) {
  return sf("0x%08x (%s)", v, v ? bit_names(v, kAudioLocations, 28).c_str() : "mono");
}
// The short form for summaries: "FL+FR", "mono".
std::string locations_short(uint32_t v) {
  if (!v) return "mono";
  std::string s;
  for (unsigned b = 0; b < 32; ++b)
    if (v >> b & 1) s += (s.empty() ? "" : "+") + std::string(b < 28 ? kAudioLocations[b] : "?");
  return s;
}

const char* lc3_freq_name(uint8_t f) {
  static const char* const names[] = {"?", "8 kHz", "11.025 kHz", "16 kHz", "22.05 kHz", "24 kHz", "32 kHz",
                                      "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "176.4 kHz", "192 kHz", "384 kHz"};
  return f < sizeof names / sizeof *names ? names[f] : "?";
}
const char* const kLc3FreqBits[] = {"8", "11.025", "16", "22.05", "24", "32", "44.1", "48", "88.2", "96",
                                    "176.4", "192", "384"};

enum class Ltv { kCodecConfig, kCodecCaps, kMetadata };

// One list of LTV structures ({length, type, value}): every one in the tree, by name with its
// value; returns the words for a summary ("48 kHz, 10 ms, 100 octets, FL"). lc3: the codec is LC3
// (Assigned Numbers 6.12.4/6.12.5 define the codec types for it; a vendor codec's are its own).
std::string ltvs(Out& o, const uint8_t* p, size_t n, Ltv kind, bool lc3 = true) {
  std::string sum;
  auto add = [&](const std::string& w) { sum += (sum.empty() ? "" : ", ") + w; };
  size_t at = 0;
  while (at < n) {
    const size_t l = p[at];
    const uint8_t* e = p + at;
    if (l == 0) {
      ++at;
      continue;
    }
    if (at + 1 + l > n) {
      F("Truncated LTV", hexbytes(e, n - at), e, n - at);
      break;
    }
    const uint8_t t = e[1];
    const uint8_t* v = e + 2;
    const size_t vn = l - 1;
    std::string name, val;
    if (kind == Ltv::kMetadata) {
      switch (t) {
        case 0x01:
        case 0x02:
          if (vn < 2) break;
          name = t == 1 ? "Preferred audio contexts" : "Streaming audio contexts";
          val = contexts_str(le16(v));
          if (t == 2) add(bit_names(le16(v), kAudioContexts, 12));
          break;
        case 0x03: name = "Program info"; val = quoted(v, vn, 200); add(quoted(v, vn, 40)); break;
        case 0x04: name = "Language"; val = quoted(v, vn, 8); add(printable(v, vn, 8)); break;
        case 0x05:
          name = "CCID list";
          for (size_t i = 0; i < vn; ++i) val += sf("%s%u", i ? ", " : "", v[i]);
          break;
        case 0x06: if (vn >= 1) { name = "Parental rating"; val = v[0] ? sf("age %u+", (v[0] & 0x0f) + 3) : "no rating"; } break;
        case 0x07: name = "Program info URI"; val = quoted(v, vn, 200); break;
        case 0x08:
          if (vn >= 1) {
            static const char* const st[] = {"no audio data", "audio data", "?"};
            name = "Audio active state";
            val = st[std::min<unsigned>(v[0], 2)];
          }
          break;
        case 0x09: name = "Broadcast audio immediate rendering"; val = "set"; break;
        case 0x0a: if (vn >= 1) { name = "Assisted listening stream"; val = v[0] ? "assisted" : "unspecified"; } break;
        case 0x0b: name = "Broadcast name"; val = quoted(v, vn, 200); add(quoted(v, vn, 40)); break;
        case 0xfe: name = "Extended metadata"; val = hexbytes(v, vn); break;
        case 0xff:
          if (vn >= 2) {
            const char* c = company_name(le16(v));
            name = "Vendor metadata";
            val = sf("company 0x%04x%s%s%s, ", le16(v), c ? " (" : "", c ? c : "", c ? ")" : "") + hexbytes(v + 2, vn - 2);
          }
          break;
        default: break;
      }
    } else if (lc3 && kind == Ltv::kCodecConfig) {
      switch (t) {
        case 0x01: if (vn >= 1) { name = "Sampling frequency"; val = sf("0x%02x (%s)", v[0], lc3_freq_name(v[0])); add(lc3_freq_name(v[0])); } break;
        case 0x02: if (vn >= 1) { name = "Frame duration"; val = v[0] == 0 ? "7.5 ms" : v[0] == 1 ? "10 ms" : "?"; add(val); } break;
        case 0x03: if (vn >= 4) { name = "Audio channel allocation"; val = locations_str(le32(v)); add(locations_short(le32(v))); } break;
        case 0x04: if (vn >= 2) { name = "Octets per codec frame"; val = std::to_string(le16(v)); add(sf("%u octets", le16(v))); } break;
        case 0x05: if (vn >= 1) { name = "Codec frame blocks per SDU"; val = std::to_string(v[0]); } break;
        default: break;
      }
    } else if (lc3) {  // capabilities
      switch (t) {
        case 0x01:
          if (vn >= 2) {
            name = "Supported sampling frequencies";
            val = sf("0x%04x (%s kHz)", le16(v), bit_names(le16(v), kLc3FreqBits, 13).c_str());
            add(bit_names(le16(v), kLc3FreqBits, 13) + " kHz");
          }
          break;
        case 0x02:
          if (vn >= 1) {
            static const char* const d[] = {"7.5 ms", "10 ms", nullptr, nullptr, "7.5 ms preferred", "10 ms preferred"};
            name = "Supported frame durations";
            val = sf("0x%02x (%s)", v[0], bit_names(v[0], d, 6).c_str());
          }
          break;
        case 0x03:
          if (vn >= 1) {
            name = "Supported audio channel counts";
            for (unsigned b = 0; b < 8; ++b)
              if (v[0] >> b & 1) val += sf("%s%u", val.empty() ? "" : ", ", b + 1);
          }
          break;
        case 0x04:
          if (vn >= 4) {
            name = "Supported octets per codec frame";
            val = sf("%u-%u", le16(v), le16(v + 2));
            add(val + " octets");
          }
          break;
        case 0x05: if (vn >= 1) { name = "Supported max codec frames per SDU"; val = std::to_string(v[0]); } break;
        default: break;
      }
    }
    if (o.detail) {
      if (name.empty()) o.add(sf("LTV type 0x%02x", t), hexbytes(v, vn), e, l + 1);
      else o.add(name, val, e, l + 1);
    }
    at += l + 1;
  }
  return sum;
}

// A codec ID, its configuration (or capabilities) and metadata, each a length then LTVs: the
// shape PAC records, ASE states and BASE subgroups share. Returns the codec and its words.
std::string codec_ltvs(Rd& r, Ltv kind, const char* len_name, const char* list_name) {
  Out& o = r.o;
  std::string sum;
  if (!r.has(5)) return sum;
  const uint8_t* id = r.cur();
  r.with("Codec", 5, [&](uint32_t) { return codec_id_str(id); });
  sum = id[0] == 0xff ? codec_id_str(id) : coding_format_name(id[0]);
  const uint8_t l = r.u8(len_name);
  if (l && r.has(1)) {
    OPEN(list_name, "", r.cur(), std::min<size_t>(l, r.left()));
    const std::string w = ltvs(o, r.cur(), std::min<size_t>(l, r.left()), kind, id[0] == 0x06);
    CLOSE();
    if (!w.empty()) sum += " " + w;
    r.skip(l);
  }
  return sum;
}

void metadata(Rd& r, const char* len_name = "Metadata length", std::string* sum = nullptr) {
  Out& o = r.o;
  const uint8_t l = r.u8(len_name);
  if (!l || !r.has(1)) return;
  OPEN("Metadata", "", r.cur(), std::min<size_t>(l, r.left()));
  const std::string w = ltvs(o, r.cur(), std::min<size_t>(l, r.left()), Ltv::kMetadata);
  CLOSE();
  if (sum && !w.empty()) *sum += (sum->empty() ? "" : ", ") + w;
  r.skip(l);
}

// The Broadcast Audio Source Endpoint structure (BAP 3.7.2.2), the Basic Audio Announcement's
// service data in periodic advertising: presentation delay, then subgroups of BISes with their
// codec configuration. Returns "2 BIS, LC3 48 kHz, 10 ms, 100 octets".
std::string base(Out& o, const uint8_t* p, size_t n) {
  Rd r(o, p, n);
  r.u24("Presentation delay", "%u us");
  const uint8_t groups = r.u8("Subgroups");
  unsigned bis_total = 0;
  std::string first;
  for (unsigned g = 0; g < groups && r.has(7); ++g) {
    OPEN(sf("Subgroup %u", g), "", r.cur(), r.left());
    const uint8_t k = r.u8("BISes");
    bis_total += k;
    const std::string c = codec_ltvs(r, Ltv::kCodecConfig, "Codec configuration length", "Codec configuration");
    std::string meta;
    metadata(r, "Metadata length", &meta);
    if (g == 0) first = c + (meta.empty() ? "" : ", " + meta);
    for (unsigned b = 0; b < k && r.has(2); ++b) {
      const uint8_t* bp = r.cur();
      OPEN(sf("BIS %u", bp[0]), "", bp, std::min<size_t>(2 + bp[1], r.left()));
      r.u8("BIS index");
      const uint8_t l = r.u8("Codec configuration length");
      if (l && r.has(1)) {
        const std::string w = ltvs(o, r.cur(), std::min<size_t>(l, r.left()), Ltv::kCodecConfig);
        if (!w.empty() && g == 0 && b == 0) first += " (BIS: " + w + ")";
        r.skip(l);
      }
      CLOSE();
    }
    CLOSE();
  }
  return sf("BASE %u BIS", bis_total) + (first.empty() ? "" : ", " + first);
}

// TMAP and GMAP roles (TMAP 4.7, GMAP 4.2), as their services advertise them.
const char* const kTmapRoles[] = {"call gateway", "call terminal", "unicast media sender", "unicast media receiver",
                                  "broadcast media sender", "broadcast media receiver"};
const char* const kGmapRoles[] = {"unicast game gateway", "unicast game terminal", "broadcast game sender",
                                  "broadcast game receiver"};

bool le_audio_service_data(uint16_t uuid) {
  return uuid == 0x1852 || uuid == 0x1851 || uuid == 0x184e || uuid == 0x1853 || uuid == 0x1856 || uuid == 0x1855 ||
         uuid == 0x1858;
}

// The service data of the LE Audio services (BAP 3.5.3, 3.7.2.1, PBP 4.1, CAP 8.1, TMAP 4.7):
// each field in the tree; returns the words for the summary ("Broadcast ID 0x34a656").
std::string service_data16(Out& o, uint16_t uuid, const uint8_t* v, size_t n) {
  Rd r(o, v, n);
  switch (uuid) {
    case 0x1852: {  // Broadcast Audio Announcement
      const uint32_t id = r.u24("Broadcast ID", "0x%06x");
      return n >= 3 ? sf("Broadcast ID 0x%06x", id) : std::string();
    }
    case 0x1851:  // Basic Audio Announcement
      return base(o, v, n);
    case 0x184e: {  // ASCS: a unicast server's announcement
      const uint8_t t = r.with("Announcement type", 1, [](uint32_t x) { return std::string(x ? "targeted" : "general"); });
      r.with("Available sink contexts", 2, [](uint32_t x) { return contexts_str(static_cast<uint16_t>(x)); });
      r.with("Available source contexts", 2, [](uint32_t x) { return contexts_str(static_cast<uint16_t>(x)); });
      metadata(r);
      return n >= 1 ? std::string(t ? "ASCS targeted" : "ASCS general") : std::string();
    }
    case 0x1853: {  // CAS: a CAP acceptor's announcement
      const uint8_t t = r.with("Announcement type", 1, [](uint32_t x) { return std::string(x ? "targeted" : "general"); });
      return n >= 1 ? std::string(t ? "CAP targeted" : "CAP general") : std::string();
    }
    case 0x1856: {  // Public Broadcast Announcement
      static const char* const f[] = {"encrypted", "standard quality", "high quality"};
      const uint8_t feat = static_cast<uint8_t>(r.flags("Features", 1, f, 3));
      std::string meta;
      metadata(r, "Metadata length", &meta);
      return n >= 1 ? "PBP " + bit_names(feat, f, 3) + (meta.empty() ? "" : ", " + meta) : std::string();
    }
    case 0x1855:  // TMAS
      r.flags("TMAP role", 2, kTmapRoles, 6);
      return {};
    case 0x1858:  // GMAS
      r.flags("GMAP role", 1, kGmapRoles, 4);
      return {};
    default:
      return {};
  }
}

// ---------------------------------------------------------------------------------------------
// Advertising / EIR data: AD structures of { length, type, data }.

const char* ad_type_name(uint8_t t) {
  switch (t) {
    case 0x01: return "Flags";
    case 0x02: return "16-bit UUIDs (incomplete)";
    case 0x03: return "16-bit UUIDs";
    case 0x04: return "32-bit UUIDs (incomplete)";
    case 0x05: return "32-bit UUIDs";
    case 0x06: return "128-bit UUIDs (incomplete)";
    case 0x07: return "128-bit UUIDs";
    case 0x08: return "Short name";
    case 0x09: return "Name";
    case 0x0a: return "TX power";
    case 0x0d: return "Class of device";
    case 0x0e: return "Simple pairing hash C";
    case 0x0f: return "Simple pairing randomizer R";
    case 0x10: return "Device ID";
    case 0x11: return "Security manager OOB flags";
    case 0x12: return "Peripheral connection interval range";
    case 0x14: return "16-bit solicitation UUIDs";
    case 0x15: return "128-bit solicitation UUIDs";
    case 0x16: return "Service data (16-bit UUID)";
    case 0x17: return "Public target address";
    case 0x18: return "Random target address";
    case 0x19: return "Appearance";
    case 0x1a: return "Advertising interval";
    case 0x1b: return "LE device address";
    case 0x1c: return "LE role";
    case 0x1f: return "32-bit solicitation UUIDs";
    case 0x20: return "Service data (32-bit UUID)";
    case 0x21: return "Service data (128-bit UUID)";
    case 0x24: return "URI";
    case 0x27: return "LE supported features";
    case 0x29: return "PB-ADV";
    case 0x2a: return "Mesh message";
    case 0x2b: return "Mesh beacon";
    case 0x2c: return "BIGInfo";
    case 0x2d: return "Broadcast code";
    case 0x2e: return "Resolvable set identifier";
    case 0x2f: return "Advertising interval (long)";
    case 0x30: return "Broadcast name";
    case 0x31: return "Encrypted advertising data";
    case 0x32: return "Periodic advertising response timing";
    case 0xff: return "Manufacturer data";
    default: return nullptr;
  }
}

// Appearance categories (Assigned Numbers 2.6.2), the upper ten bits.
const char* appearance_name(uint16_t a) {
  switch (a >> 6) {
    case 0: return "unknown";
    case 1: return "phone";
    case 2: return "computer";
    case 3: return "watch";
    case 4: return "clock";
    case 5: return "display";
    case 6: return "remote control";
    case 7: return "eye-glasses";
    case 8: return "tag";
    case 9: return "keyring";
    case 10: return "media player";
    case 11: return "barcode scanner";
    case 12: return "thermometer";
    case 13: return "heart rate sensor";
    case 14: return "blood pressure";
    case 15: return "human interface device";
    case 16: return "glucose meter";
    case 17: return "running walking sensor";
    case 18: return "cycling";
    case 19: return "control device";
    case 20: return "network device";
    case 21: return "sensor";
    case 22: return "light fixtures";
    case 31: return "light source";
    case 33: return "audio sink";
    case 34: return "audio source";
    case 37: return "wearable audio device";
    case 39: return "AV equipment";
    case 40: return "display equipment";
    case 41: return "hearing aid";
    case 42: return "gaming";
    default: return "?";
  }
}

std::string cod_str(uint32_t c);

// One AD structure's value in the tree: a leaf, or a node with the fields of an LE Audio service's
// data. *info gets the words the summary shows besides the name (a Broadcast ID, a BASE).
void ad_struct(Out& o, uint8_t t, const uint8_t* e, size_t el, std::string* info) {
  const uint8_t* v = e + 2;
  const size_t vn = el - 2;
  const char* tn = ad_type_name(t);
  const std::string name = tn ? tn : sf("AD type 0x%02x", t);
  std::string val;
  switch (t) {
    case 0x01: {
      static const char* const fl[] = {"LE limited discoverable", "LE general discoverable", "BR/EDR not supported",
                                       "LE and BR/EDR (controller)", "LE and BR/EDR (host)"};
      if (vn >= 1) val = sf("0x%02x (%s)", v[0], bit_names(v[0], fl, 5).c_str());
      break;
    }
    case 0x02:
    case 0x03:
    case 0x14:
      for (size_t i = 0; i + 1 < vn; i += 2) val += (val.empty() ? "" : ", ") + uuid_str(v + i, 2);
      break;
    case 0x04:
    case 0x05:
    case 0x1f:
      for (size_t i = 0; i + 3 < vn; i += 4) val += (val.empty() ? "" : ", ") + uuid_str(v + i, 4);
      break;
    case 0x06:
    case 0x07:
    case 0x15:
      for (size_t i = 0; i + 15 < vn; i += 16) val += (val.empty() ? "" : ", ") + uuid_str(v + i, 16);
      break;
    case 0x08:
    case 0x09:
    case 0x30:
      val = "\"" + printable(v, vn) + "\"";
      break;
    case 0x0a:
      if (vn >= 1) val = sf("%d dBm", static_cast<int8_t>(v[0]));
      break;
    case 0x0d:
      if (vn >= 3) val = cod_str(le24(v));
      break;
    case 0x10:
      if (vn >= 8) {
        const char* c = le16(v) == 1 ? company_name(le16(v + 2)) : nullptr;
        val = sf("%s vendor 0x%04x%s%s%s, product 0x%04x, version 0x%04x", le16(v) == 1 ? "Bluetooth SIG" : le16(v) == 2 ? "USB" : "?",
                 le16(v + 2), c ? " (" : "", c ? c : "", c ? ")" : "", le16(v + 4), le16(v + 6));
      }
      break;
    case 0x12:
      if (vn >= 4) val = sf("%.2f-%.2f ms", le16(v) * 1.25, le16(v + 2) * 1.25);
      break;
    case 0x17:
    case 0x18:
      for (size_t i = 0; i + 5 < vn; i += 6) val += (val.empty() ? "" : ", ") + bdaddr_str(v + i);
      break;
    case 0x19:
      if (vn >= 2) val = sf("0x%04x (%s)", le16(v), appearance_name(le16(v)));
      break;
    case 0x1a:
      if (vn >= 2) val = sf("%.2f ms", le16(v) * 0.625);
      break;
    case 0x2f:
      if (vn >= 3) val = sf("%.2f ms", (vn >= 4 ? le32(v) : le24(v)) * 0.625);
      break;
    case 0x1b:
      if (vn >= 7) val = bdaddr_str(v) + (v[6] & 1 ? " (random)" : " (public)");
      break;
    case 0x1c: {
      static const char* const roles[] = {"peripheral only", "central only", "both, peripheral preferred",
                                          "both, central preferred"};
      if (vn >= 1) val = v[0] < 4 ? roles[v[0]] : "?";
      break;
    }
    case 0x24:
      val = vn >= 1 ? sf("scheme 0x%02x ", v[0]) + quoted(v + 1, vn - 1, 200) : std::string();
      break;
    case 0x27:
      if (vn >= 1) {
        uint8_t f[8] = {};
        std::memcpy(f, v, std::min<size_t>(vn, 8));
        val = le_features_str(f);
      }
      break;
    case 0xff:
      if (vn >= 2) {
        const char* c = company_name(le16(v));
        val = sf("company 0x%04x%s%s%s, ", le16(v), c ? " (" : "", c ? c : "", c ? ")" : "") + hexbytes(v + 2, vn - 2);
      }
      break;
    case 0x16:
      if (vn >= 2) {
        const uint16_t u = le16(v);
        // LE Audio's announcements get a node of their own with every field; other services'
        // data stays bytes.
        if (le_audio_service_data(u)) {
          OPEN(name, uuid_str(v, 2), e, el);
          const std::string w = service_data16(o, u, v + 2, vn - 2);
          rest(o, v + 2, vn - 2, "Data");
          CLOSE();
          if (info && !w.empty()) *info += (info->empty() ? "" : ", ") + w;
          return;
        }
        val = uuid_str(v, 2) + (vn > 2 ? ": " + hexbytes(v + 2, vn - 2) : std::string());
      }
      break;
    case 0x20:
      if (vn >= 4) val = uuid_str(v, 4) + ": " + hexbytes(v + 4, vn - 4);
      break;
    case 0x21:
      if (vn >= 16) val = uuid_str(v, 16) + ": " + hexbytes(v + 16, vn - 16);
      break;
    default:
      val = hexbytes(v, vn);
  }
  o.add(name, val, e, el);
}

// Returns the local name, if the data carries one; in detail mode lists every structure. *info,
// when given, gets what the LE Audio announcements say (Broadcast ID, the BASE's streams); the
// summary pays for that only when the data has one.
std::string ad_data(Out& o, const uint8_t* p, size_t n, std::string* info) {
  std::string name;
  while (n >= 2) {
    const size_t l = p[0];
    if (l == 0) break;  // EIR is zero-padded to 240 bytes
    if (l + 1 > n) break;
    const uint8_t t = p[1];
    const uint8_t* v = p + 2;
    const size_t vn = l - 1;
    if ((t == 0x08 || t == 0x09 || t == 0x30) && (name.empty() || t == 0x09)) {
      name = printable(v, vn, 40);
    }
    if (o.detail) {
      ad_struct(o, t, p, l + 1, info);
    } else if (info && t == 0x16 && vn >= 2 && le_audio_service_data(le16(v))) {
      const std::string w = service_data16(o, le16(v), v + 2, vn - 2);
      if (!w.empty()) *info += (info->empty() ? "" : ", ") + w;
    }
    p += l + 1;
    n -= l + 1;
  }
  return name;
}

std::string ad_data(Out& o, const uint8_t* p, size_t n) { return ad_data(o, p, n, nullptr); }

// The LE Extended Create Connection parameters of one initiating PHY.
void ext_conn_phy(Rd& r, const char* phy) {
  OPEN_R(r, phy, "", r.cur(), std::min<size_t>(16, r.left()));
  r.time("Scan interval", 2, 0.625);
  r.time("Scan window", 2, 0.625);
  r.time("Min connection interval", 2, 1.25);
  r.time("Max connection interval", 2, 1.25);
  r.u16("Peripheral latency");
  r.time("Supervision timeout", 2, 10);
  r.time("Min CE length", 2, 0.625);
  r.time("Max CE length", 2, 0.625);
  CLOSE_R(r);
}

// The LE Extended Advertising/Scan Response Data, Periodic Advertising Data: AD structures, with
// the LE Audio announcements decoded. Appends the name and what an announcement says to *s.
void adv_data_block(Out& o, const uint8_t* p, size_t n, std::string* s) {
  OPEN("Advertising data", sf("%zu bytes", n), p, n);
  std::string info;
  const std::string name = ad_data(o, p, n, &info);
  CLOSE();
  if (!name.empty()) *s += " \"" + name + "\"";
  if (!info.empty()) *s += " " + info;
}

// ---------------------------------------------------------------------------------------------
// Intel vendor commands: what btintel sends a BE200 and its relatives (the firmware download in
// Secure Send fragments, the DDC entries written after it, the version and feature reads) and the
// Microsoft extension. Their layouts are those of btmon's monitor/intel.c and msft.c, and of
// btintel.c for the commands btmon leaves undecoded.

const char* intel_variant_name(uint32_t v) {
  switch (v) {
    case 0x17: return "Typhoon Peak2";
    case 0x18: return "Solar";
    case 0x19: return "Solar F";
    case 0x1b: return "Magnetor";
    case 0x1c: return "Gale Peak2";
    case 0x1d: return "BlazarU";
    case 0x1e: return "BlazarI";
    case 0x1f: return "Scorpius Peak";
    case 0x22: return "BlazarIW";
    default: return nullptr;
  }
}

const char* intel_image_name(uint32_t v) {
  return v == 0x01 ? "bootloader" : v == 0x02 ? "IML" : v == 0x03 ? "firmware" : "?";
}

// The TLVs of Read Version (every controller since Typhoon Peak), types 16 to 49.
const char* intel_tlv_name(uint8_t t) {
  static const char* const names[] = {
      "CNVi TOP", "CNVr TOP", "CNVi BT", "CNVr BT", "CNVi OTP", "CNVr OTP", "Device revision ID",
      "USB VID", "USB PID", "PCIe VID", "PCIe DID", "PCIe subsystem ID", "Image type", "Time stamp",
      "Build type", "Build number", "FW build product", "FW build HW", "FW build step", "BT spec",
      "Manufacturer", "HCI revision", "LMP subversion", "OTP patch version", "Secure boot",
      "Key from header", "OTP lock", "API lock", "Debug lock", "Minimum FW", "Limited CCE", "SBE type",
      "OTP BD_ADDR", "Unlocked state"};
  return t >= 16 && t - 16u < sizeof names / sizeof *names ? names[t - 16] : nullptr;
}

std::string intel_tlv_value(uint8_t t, const uint8_t* v, size_t n) {
  switch (t) {
    case 18:  // CNVi BT: the variant is bits 16-21
      if (n >= 4) {
        const char* name = intel_variant_name(le32(v) >> 16 & 0x3f);
        return sf("0x%08x", le32(v)) + (name ? std::string(" (") + name + ")" : "");
      }
      break;
    case 28:
      if (n >= 1) return sf("%s (0x%02x)", intel_image_name(v[0]), v[0]);
      break;
    case 29:  // week, year: what btintel prints as "Firmware timestamp 20yy.ww"
      if (n >= 2) return sf("20%02u.%u", v[1], v[0]);
      break;
    case 31:
      if (n >= 4) return std::to_string(le32(v));
      break;
    case 36:
      if (n >= 2) return company_str(le16(v));
      break;
    case 40: case 41: case 42: case 43: case 44: case 46: case 49:
      if (n >= 1) return sf("%s (0x%02x)", v[0] ? "enabled" : "disabled", v[0]);
      break;
    case 45:
      if (n >= 3) return sf("%u-%u.%u", v[0], v[1], 2000 + v[2]);
      break;
    case 48:
      if (n >= 6) return bdaddr_str(v);
      break;
    default:
      break;
  }
  if (n == 1) return sf("0x%02x", v[0]);
  if (n == 2) return sf("0x%04x", le16(v));
  if (n == 4) return sf("0x%08x", le32(v));
  return hexbytes(v, n, 64);
}

// The DDC (device configuration) entries named so far: LE features is where the BE200's firmware
// keeps Auracast off (docs/audio.md), LE extended features is btmon's.
const char* intel_ddc_name(uint16_t id) {
  switch (id) {
    case 0x0126: return "LE features";
    case 0x031f: return "LE extended features";
    default: return nullptr;
  }
}

// Set Event Mask's bits (btmon's events_table).
const char* const kIntelEvents[] = {"bootup", "SCO rejected via LMP", "PTT switch notification", nullptr,
                                    nullptr, nullptr, nullptr, "scan status", nullptr, "debug exception",
                                    "fatal exception", "system exception", nullptr, "LE link established",
                                    "FW trace string"};

const char* msft_name(uint32_t code) {
  switch (code) {
    case 0x00: return "Read Supported Features";
    case 0x01: return "Monitor RSSI";
    case 0x02: return "Cancel Monitor RSSI";
    case 0x03: return "LE Monitor Advertisement";
    case 0x04: return "LE Cancel Monitor Advertisement";
    case 0x05: return "LE Set Advertisement Filter Enable";
    case 0x06: return "Read Absolute RSSI";
    default: return "?";
  }
}

const char* const kMsftFeatures[] = {
    "RSSI monitoring for BR/EDR", "RSSI monitoring for LE connections", "RSSI monitoring of LE advertisements",
    "advertisement monitoring", "P-192/P-256 key validation", "continuous advertisement monitoring v1",
    nullptr, "AVDTP offload", nullptr, nullptr, "continuous advertisement monitoring v2"};

// A 64-bit little-endian bit field of the parameters, named from a table.
void bits64(Rd& r, const char* name, const char* const* names, size_t count) {
  if (!r.has(8)) return r.skip(8);
  const uint8_t* p = r.cur();
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
  if (r.o.detail) r.o.add(name, sf("0x%016llx (%s)", static_cast<unsigned long long>(v), bit_names(v, names, count).c_str()), p, 8);
  r.skip(8);
}

void msft_rssi_params(Rd& r) {
  r.dbm("RSSI threshold high");
  r.dbm("RSSI threshold low");
  r.u8("RSSI threshold low interval", "%u s");
  r.with("RSSI sampling period", 1, [](uint32_t v) { return sf("%u ms (0x%02x)", v * 100, v); });
}

std::string msft_cmd(Rd& r) {
  if (!r.has(1)) return {};
  const uint8_t code = static_cast<uint8_t>(r.named("Subcommand", 1, msft_name));
  std::string s = std::string(" ") + msft_name(code);
  switch (code) {
    case 0x01:  // Monitor RSSI
      s += sf(" handle %u", r.handle());
      msft_rssi_params(r);
      break;
    case 0x02:  // Cancel Monitor RSSI
    case 0x06:  // Read Absolute RSSI
      s += sf(" handle %u", r.handle());
      break;
    case 0x03: {  // LE Monitor Advertisement: thresholds, then what to match
      msft_rssi_params(r);
      const uint8_t type = static_cast<uint8_t>(r.named("Condition type", 1, [](uint32_t v) {
        return v == 1 ? "patterns" : v == 2 ? "UUID" : v == 3 ? "IRK" : v == 4 ? "address" : "?";
      }));
      if (type == 1) {
        const uint8_t num = r.u8("Patterns");
        s += sf(" %u pattern%s", num, num == 1 ? "" : "s");
        for (unsigned i = 0; i < num && r.has(1); ++i) {
          const uint8_t len = static_cast<uint8_t>(r.raw(1));
          OPEN_R(r, "Pattern", sf("%u", i + 1), r.cur(), std::min<size_t>(1 + len, r.left()));
          r.u8("Length");
          r.named("AD type", 1, [](uint32_t v) { return ad_type_name(static_cast<uint8_t>(v)); });
          r.u8("Start byte");
          r.hex("Data", len >= 2 ? len - 2 : 0);
          CLOSE_R(r);
        }
      } else if (type == 2 && r.has(1)) {
        const uint8_t ut = r.u8("UUID type");
        const size_t k = ut == 1 ? 2 : ut == 2 ? 4 : ut == 3 ? 16 : 0;
        if (k && r.has(k)) {
          const std::string u = uuid_str(r.cur(), k);
          if (r.o.detail) r.o.add("UUID", u, r.cur(), k);
          r.skip(k);
          s += " " + u;
        }
      } else if (type == 3) {
        r.hex("IRK", 16);
      } else if (type == 4) {
        r.atype();
        s += " " + r.addr();
      }
      break;
    }
    case 0x04:  // LE Cancel Monitor Advertisement
      s += sf(" monitor %u", r.u8("Monitor handle"));
      break;
    case 0x05:  // LE Set Advertisement Filter Enable
      s += r.with("Enable", 1, [](uint32_t v) {
             return sf("%s (0x%02x)", v == 0 ? "current allow list" : v == 1 ? "all filter conditions" : "?", v);
           }) ? " on" : " off";
      break;
    default:
      break;
  }
  return s;
}

// After the status: the subcommand, then its return parameters.
std::string msft_return(Rd& r) {
  if (!r.has(1)) return {};
  const uint8_t code = static_cast<uint8_t>(r.named("Subcommand", 1, msft_name));
  std::string s = std::string(" ") + msft_name(code);
  switch (code) {
    case 0x00: {  // Read Supported Features
      bits64(r, "Features", kMsftFeatures, sizeof kMsftFeatures / sizeof *kMsftFeatures);
      const uint8_t len = r.u8("Event prefix length");
      r.hex("Event prefix", len);
      break;
    }
    case 0x03:  // LE Monitor Advertisement
      s += sf(" monitor %u", r.u8("Monitor handle"));
      break;
    case 0x06:  // Read Absolute RSSI
      s += sf(" handle %u", r.handle());
      r.dbm("RSSI");
      break;
    default:
      break;
  }
  return s;
}

// The parameters of an Intel vendor command; returns what the summary adds to its name.
std::string intel_cmd(Rd& r, uint16_t op) {
  std::string s;
  switch (op) {
    case 0xfc01:  // Reset
      r.named("Reset type", 1, [](uint32_t v) { return v == 0 ? "soft" : v == 1 ? "hard" : "?"; });
      r.yes_no("Patch vectors");
      r.named("DDC parameters", 1, [](uint32_t v) { return v == 0 ? "keep" : v == 1 ? "reload from OTP" : "?"; });
      if (r.named("Boot option", 1, [](uint32_t v) { return v == 0 ? "current image" : v == 1 ? "boot address" : "?"; })) {
        s += sf(" boot 0x%08x", r.u32("Boot address", "0x%08x"));
      } else {
        r.u32("Boot address", "0x%08x");
      }
      break;
    case 0xfc05:  // Read Version: none for the legacy layout, else the TLV types wanted
      if (r.has(1) && r.cur()[0] == 0xff) s += " all types";
      while (r.has(1)) {
        r.with("Requested type", 1, [](uint32_t v) {
          const char* n = v == 0xff ? "all supported" : intel_tlv_name(static_cast<uint8_t>(v));
          return sf("%s (0x%02x)", n ? n : "?", v);
        });
      }
      break;
    case 0xfc06: {  // Set UART Baudrate
      static const char* const rates[] = {"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600",
                                          "1843200", "3250000", "2000000", "3000000", "3714286", "4333333", "6500000"};
      r.with("Baudrate", 1, [](uint32_t v) { return sf("%s (0x%02x)", v < 15 ? rates[v] : "?", v); });
      break;
    }
    case 0xfc09: {  // Secure Send: one fragment of the firmware file
      auto type = [](uint32_t v) { return v == 0 ? "init" : v == 1 ? "data" : v == 2 ? "sign" : v == 3 ? "public key" : "?"; };
      const char* t = type(r.named("Type", 1, type));
      s += sf(" %s %zu bytes", t, r.left());
      r.hex("Fragment", r.left());
      break;
    }
    case 0xfc0a:  // Set DSBR (btintel_set_dsbr)
      r.yes_no("Enable");
      r.u8("DSBR", "0x%02x");
      break;
    case 0xfc0e:  // Write Secure Boot Params, from the firmware file
      s += sf(" boot 0x%08x", r.u32("Boot address", "0x%08x"));
      r.u8("Firmware build number");
      r.u8("Firmware build week");
      r.u8("Firmware build year", "20%02u");
      break;
    case 0xfc11:  // Manufacturer Mode
      s += r.yes_no("Mode") ? " on" : " off";
      r.named("Reset", 1, [](uint32_t v) {
        return v == 0 ? "no reset" : v == 1 ? "reset, patches off" : v == 2 ? "reset, patches on" : "?";
      });
      break;
    case kIntelMsft:
      s += msft_cmd(r);
      break;
    case 0xfc2f:  // Write BD Data
      s += " " + r.addr();
      break;
    case 0xfc31:  // Write BD Address
      s += " " + r.addr();
      break;
    case 0xfc43:  // Activate Deactivate Traces
      r.u8("Transmit traces", "0x%02x");
      r.u8("Transmit ARQ", "0x%02x");
      r.u8("Receive traces", "0x%02x");
      break;
    case 0xfc4d:  // Stimulate Exception
      r.named("Type", 1, [](uint32_t v) { return v == 0 ? "fatal exception" : v == 1 ? "debug exception" : "?"; });
      break;
    case 0xfc52:  // Set Event Mask
      bits64(r, "Mask", kIntelEvents, sizeof kIntelEvents / sizeof *kIntelEvents);
      break;
    case 0xfc8b: {  // DDC Config Write: entries of length (of id and value), id, value
      std::string ids;
      while (r.has(3)) {
        const uint8_t len = static_cast<uint8_t>(r.raw(1));
        if (len < 2 || !r.has(1 + len)) break;
        const uint16_t id = le16(r.cur() + 1);
        const char* name = intel_ddc_name(id);
        OPEN_R(r, "DDC entry", sf("0x%04x", id) + (name ? std::string(" (") + name + ")" : ""), r.cur(), 1 + len);
        r.u8("Length");
        r.u16("Identifier", "0x%04x");
        if (id == 0x0126 && len - 2 >= 8) {
          const std::string f = le_features_str(r.cur());
          if (r.o.detail) r.o.add("LE features", hexbytes(r.cur(), 8) + " (" + f + ")", r.cur(), 8);
          r.skip(8);
          r.hex("Value", len - 10);
        } else {
          r.hex("Value", len - 2);
        }
        CLOSE_R(r);
        ids += sf(" 0x%04x", id);
      }
      s += ids;
      break;
    }
    case 0xfc8c:  // DDC Config Read: the entry's id
      if (r.has(2)) s += sf(" 0x%04x", r.u16("Identifier", "0x%04x"));
      break;
    case 0xfc8e:  // Memory Write
      s += sf(" 0x%08x", r.u32("Address", "0x%08x"));
      r.named("Mode", 1, [](uint32_t v) { return v == 0 ? "byte" : v == 1 ? "half word" : v == 2 ? "word" : "?"; });
      r.u8("Length");
      r.hex("Data", r.left());
      break;
    case 0xfca1:  // btintel_set_debug_features: 0x02 on, 0x00 off
      r.u8("Trace enable", "0x%02x");
      break;
    case 0xfca6:  // Read Supported Features
      r.u8("Page");
      break;
    case 0xfe0b:  // PPAG Enable
      r.with("Enable", 4, [](uint32_t v) {
        return sf("%s (0x%08x)", v == 1 ? "EU" : v == 2 ? "China" : v == 3 ? "EU and China" : "?", v);
      });
      break;
    default:
      break;
  }
  return s;
}

// The return parameters after a successful status.
std::string intel_return(Rd& r, uint16_t op, size_t n) {
  std::string s;
  switch (op) {
    case 0xfc05: {  // Read Version: btmon's test for the legacy layout (status and 9 bytes)
      if (n == 10 || (r.has(1) && r.cur()[0] == 0x37)) {
        r.u8("Hardware platform", "0x%02x");
        const uint8_t hw = r.u8("Hardware variant", "0x%02x");
        r.with("Hardware revision", 1, [](uint32_t v) { return sf("%u.%u", v >> 4, v & 0x0f); });
        const uint8_t fw = r.u8("Firmware variant", "0x%02x");
        r.with("Firmware revision", 1, [](uint32_t v) { return sf("%u.%u", v >> 4, v & 0x0f); });
        if (r.has(3)) {
          const uint8_t* b = r.cur();
          if (r.o.detail) r.o.add("Firmware build", sf("%u-%u.%u", b[0], b[1], 2000 + b[2]), b, 3);
          r.skip(3);
        }
        r.u8("Firmware patch");
        s += sf(" hw variant 0x%02x fw variant 0x%02x", hw, fw);
        break;
      }
      const char* variant = nullptr;
      const char* image = nullptr;
      while (r.has(2)) {
        const uint8_t* t = r.cur();
        if (!r.has(2u + t[1])) break;
        const char* name = intel_tlv_name(t[0]);
        if (r.o.detail) r.o.add(name ? name : sf("Type %u", t[0]), intel_tlv_value(t[0], t + 2, t[1]), t, 2u + t[1]);
        if (t[0] == 18 && t[1] >= 4) variant = intel_variant_name(le32(t + 2) >> 16 & 0x3f);
        if (t[0] == 28 && t[1] >= 1) image = intel_image_name(t[2]);
        r.skip(2u + t[1]);
      }
      if (variant) s += std::string(" ") + variant;
      if (image) s += std::string(" ") + image;
      break;
    }
    case 0xfc0d:  // Read Secure Boot Params (btintel's struct intel_boot_params)
      r.u8("OTP format", "0x%02x");
      r.u8("OTP content", "0x%02x");
      r.u8("OTP patch", "0x%02x");
      r.u16("Device revision ID", "0x%04x");
      r.yes_no("Secure boot");
      r.yes_no("Key from header");
      r.u8("Key type", "0x%02x");
      r.yes_no("OTP lock");
      r.yes_no("API lock");
      r.yes_no("Debug lock");
      r.addr("OTP BD_ADDR");
      if (r.has(3)) {
        const uint8_t* b = r.cur();
        if (r.o.detail) r.o.add("Minimum firmware build", sf("%u-%u.%u", b[0], b[1], 2000 + b[2]), b, 3);
        r.skip(3);
      }
      r.yes_no("Limited CCE");
      r.yes_no("Unlocked state");
      break;
    case kIntelMsft:
      s += msft_return(r);
      break;
    case 0xfc30:  // Read BD Data
      s += " " + r.addr();
      break;
    case 0xfc86:  // Read Offload Use Cases: btintel turns on HFP offload if preset[0] & 0x03
      r.hex("Use cases", 8);
      break;
    case 0xfc8b:  // DDC Config Write
      r.u16("Identifier", "0x%04x");
      break;
    case 0xfca6:  // Read Supported Features
      r.u8("Page");
      r.u8("Max pages");
      r.hex("Features", 16);
      break;
    case 0xfcb3: {  // Read FSEQ Status (btintel_print_fseq_info)
      const uint8_t st = static_cast<uint8_t>(r.named("FSEQ status", 1, [](uint32_t v) {
        return v == 0 ? "success" : v == 1 ? "fatal error" : v == 2 ? "semaphore acquire error" : "?";
      }));
      if (st) s += " fseq error";
      r.u32("Reason", "0x%08x");
      r.u32("Global version", "0x%08x");
      r.u32("Installed version", "0x%08x");
      for (const char* name : {"Executed", "BT top"}) {
        if (!r.has(4)) break;
        const uint8_t* b = r.cur();
        if (r.o.detail) r.o.add(name, sf("%02u.%02u.%02u.%02u", b[0], b[1], b[2], b[3]), b, 4);
        r.skip(4);
      }
      for (const char* name : {"Top init version", "CNVio init version", "MBX WiFi file version", "BT version",
                               "Top reset address", "MBX timeout", "MBX ack", "CNVi ID", "CNVr ID", "Error handle",
                               "Magic no-alive indication", "OTP version", "MBX OTP version"}) {
        r.u32(name, "0x%08x");
      }
      break;
    }
    default:
      break;
  }
  return s;
}

// Intel's vendor events (HCI event 0xff): the first byte names the event, except for the ones
// behind the vendor prefix 0x87 0x80, which Intel's controllers give both the Microsoft
// extension's events (0x01, 0x02: the prefix its Read Supported Features reports) and their own
// telemetry (0x03), as btmon's packet.c and intel.c tell them apart.
const char* intel_evt_name(uint8_t e) {
  switch (e) {
    case 0x00: return "Startup";
    case 0x01: return "Fatal Exception";
    case 0x02: return "Bootup";
    case 0x05: return "Default BD Data";
    case 0x06: return "Secure Send Commands Result";
    case 0x08: return "Debug Exception";
    case 0x0f: return "LE Link Established";
    case 0x11: return "Scan Status";
    case 0x16: return "Activate Deactivate Traces Complete";
    case 0x17: return "LMP PDU Trace";
    case 0x19: return "Write BD Data Complete";
    case 0x25: return "SCO Rejected via LMP";
    case 0x26: return "PTT Switch Notification";
    case 0x29: return "System Exception";
    case 0x2c: return "FW Trace String";
    case 0x2e: return "FW Trace Binary";
    default: return nullptr;
  }
}

const char* intel_module_name(uint32_t m) {
  static const char* const names[] = {nullptr, "BC", "HCI", "LLC", "OS", "LM", "SC", "SP", "OSAL", "LC", "APP", "TLD"};
  if (m == 0xf0) return "debug";
  return m < sizeof names / sizeof *names && names[m] ? names[m] : "?";
}

// The telemetry TLVs (btmon's intel_ext_subevent_table): the link quality counters of an ACL or
// SCO/eSCO link that the kernel's quality report asks for.
const char* intel_telemetry_name(uint8_t id) {
  switch (id) {
    case 0x01: return "Event type";
    case 0x4a: return "ACL handle";
    case 0x4b: return "RX HEC errors";
    case 0x4c: return "RX CRC errors";
    case 0x4d: return "Packets from host";
    case 0x4e: return "TX packets";
    case 0x4f: return "TX packets, 0 retries";
    case 0x50: return "TX packets, 1 retry";
    case 0x51: return "TX packets, 2 retries";
    case 0x52: return "TX packets, 3 retries";
    case 0x53: return "TX packets, 4 or more retries";
    case 0x54: return "TX DH1 packets";
    case 0x55: return "TX DH3 packets";
    case 0x56: return "TX DH5 packets";
    case 0x57: return "TX 2-DH1 packets";
    case 0x58: return "TX 2-DH3 packets";
    case 0x59: return "TX 2-DH5 packets";
    case 0x5a: return "TX 3-DH1 packets";
    case 0x5b: return "TX 3-DH3 packets";
    case 0x5c: return "TX 3-DH5 packets";
    case 0x5d: return "RX packets";
    case 0x5e: return "Link throughput";
    case 0x5f: return "Max packet latency";
    case 0x60: return "Average packet latency";
    case 0x61: return "RX RSSI moving average";
    case 0x62: return "RX SNR bad margin count";
    case 0x63: return "RX RSSI bad count";
    case 0x64: return "TX RSSI bad count";
    case 0x6a: return "SCO handle";
    case 0x6b: return "RX packets";
    case 0x6c: return "TX packets";
    case 0x6d: return "RX payloads lost";
    case 0x6e: return "TX payloads lost";
    case 0x6f: return "RX no sync errors";
    case 0x70: return "RX HEC errors";
    case 0x71: return "RX CRC errors";
    case 0x72: return "RX NAKs";
    case 0x73: return "TX failed by Wi-Fi coex";
    case 0x74: return "RX failed by Wi-Fi coex";
    case 0x75: return "Late samples inserted";
    case 0x76: return "Samples dropped";
    case 0x77: return "Mute samples at connection";
    case 0x78: return "PLC injection data";
    default: return nullptr;
  }
}

const char* intel_telemetry_type(uint32_t t) {
  switch (t) {
    case 0x00: return "system exception";
    case 0x01: return "fatal exception";
    case 0x02: return "debug exception";
    case 0x03: return "BR/EDR connection";
    case 0x04: return "disconnection";
    case 0x05: return "performance stats";
    default: return "?";
  }
}

std::string intel_telemetry_value(uint8_t id, const uint8_t* v, size_t n) {
  if (id == 0x01 && n == 1) return sf("%s (0x%02x)", intel_telemetry_type(v[0]), v[0]);
  if ((id == 0x4a || id == 0x6a || id == 0x61) && n == 2) return std::to_string(id == 0x61 ? static_cast<int16_t>(le16(v)) : le16(v));
  if (id >= 0x62 && id <= 0x64 && n == 12) return sf("1M %u, 2M %u, 3M %u", le32(v), le32(v + 4), le32(v + 8));
  if (id >= 0x6f && id <= 0x74 && n == 20) {
    return sf("%u %u %u %u %u", le32(v), le32(v + 4), le32(v + 8), le32(v + 12), le32(v + 16));
  }
  if (n == 4) return sf(id == 0x5e ? "%u bps" : id == 0x5f || id == 0x60 ? "%u us" : "%u", le32(v));
  return hexbytes(v, n, 64);
}

// The parameters of an Intel vendor event (after the event code and length); returns its summary.
std::string intel_event(Rd& r) {
  if (!r.has(1)) return "Intel Vendor";
  const uint8_t* p = r.cur();
  if (r.has(3) && p[0] == 0x87 && p[1] == 0x80 && p[2] >= 0x01 && p[2] <= 0x03) {
    r.hex("Vendor prefix", 2);
    const uint8_t code = static_cast<uint8_t>(r.named("Subevent", 1, [](uint32_t v) {
      return v == 1 ? "MSFT RSSI Event" : v == 2 ? "MSFT LE Monitor Device" : "Intel Extended Telemetry";
    }));
    if (code == 0x02) {  // MSFT LE Monitor Device: a monitored device came in or left range
      r.atype();
      const std::string a = r.addr();
      const uint8_t h = r.u8("Monitor handle");
      const uint8_t st = static_cast<uint8_t>(r.named("State", 1, [](uint32_t v) {
        return v == 0 ? "stop monitoring" : v == 1 ? "start monitoring" : "?";
      }));
      return sf("MSFT LE Monitor Device %s monitor %u %s", a.c_str(), h, st ? "start" : "stop");
    }
    if (code == 0x01) return "MSFT RSSI Event";
    std::string type, handle;
    while (r.has(2)) {
      const uint8_t* t = r.cur();
      if (!r.has(2u + t[1])) break;
      const char* name = intel_telemetry_name(t[0]);
      if (r.o.detail) r.o.add(name ? name : sf("Subevent 0x%02x", t[0]), intel_telemetry_value(t[0], t + 2, t[1]), t, 2u + t[1]);
      if (t[0] == 0x01 && t[1] == 1) type = intel_telemetry_type(t[2]);
      if ((t[0] == 0x4a || t[0] == 0x6a) && t[1] == 2) handle = sf(" %s handle %u", t[0] == 0x4a ? "ACL" : "SCO", le16(t + 2));
      r.skip(2u + t[1]);
    }
    return "Intel Extended Telemetry" + (type.empty() ? "" : " " + type) + handle;
  }
  const uint8_t code = static_cast<uint8_t>(r.named("Event", 1, [](uint32_t v) {
    const char* n = intel_evt_name(static_cast<uint8_t>(v));
    return n ? n : "?";
  }));
  const char* name = intel_evt_name(code);
  std::string s = name ? std::string("Intel ") + name : sf("Intel Vendor 0x%02x", code);
  switch (code) {
    case 0x01:  // Fatal Exception
    case 0x08: {  // Debug Exception: where in the firmware
      const uint16_t line = r.u16("Line");
      const char* module = intel_module_name(r.has(1) ? r.cur()[0] : 0);
      r.named("Module", 1, intel_module_name);
      const uint8_t reason = r.u8("Reason", "0x%02x");
      s += sf(" %s line %u reason 0x%02x", module, line, reason);
      break;
    }
    case 0x02: {  // Bootup
      r.u8("Zero", "0x%02x");
      r.u8("Number of packets");
      const uint8_t src = static_cast<uint8_t>(r.named("Source", 1, [](uint32_t v) {
        return v == 0 ? "bootloader" : v == 1 ? "operational firmware" : v == 2 ? "self test firmware" : "?";
      }));
      r.named("Reset type", 1, [](uint32_t v) {
        static const char* const t[] = {"hardware reset", "soft watchdog reset", "soft software reset",
                                        "hard watchdog reset", "hard software reset"};
        return v < 5 ? t[v] : "?";
      });
      auto reason = [](uint32_t v) {
        static const char* const t[] = {"power on", "reset command", "Intel reset command", "watchdog",
                                        "fatal exception", "system exception"};
        return v < 6 ? t[v] : v == 0xff ? "unknown" : "?";
      };
      const char* why = r.has(1) ? reason(r.cur()[0]) : nullptr;
      r.named("Reset reason", 1, reason);
      r.named("DDC status", 1, [](uint32_t v) {
        static const char* const t[] = {"firmware default", "firmware default plus OTP", "persistent RAM", "not used"};
        return v < 4 ? t[v] : "?";
      });
      s += src == 0 ? " bootloader" : src == 1 ? " firmware" : src == 2 ? " self test" : "";
      if (why) s += std::string(" (") + why + ")";
      break;
    }
    case 0x05:  // Default BD Data
      r.named("Memory status", 1, [](uint32_t v) { return v == 2 ? "invalid manufacturing data" : "?"; });
      break;
    case 0x06: {  // Secure Send Commands Result: how the firmware file's commands went
      static const char* const results[] = {"success", "general failure", "hardware failure",
                                            "signature verification failed", "parsing error of command buffer",
                                            "command execution failure", "command parameters error", "command missing"};
      const uint8_t res = static_cast<uint8_t>(r.named("Result", 1, [](uint32_t v) { return v < 8 ? results[v] : "?"; }));
      const uint16_t op = r.u16("Opcode", "0x%04x");
      r.status();
      s += std::string(" ") + (res < 8 ? results[res] : "?");
      if (res) s += sf(" opcode 0x%04x", op);
      break;
    }
    case 0x0f:  // LE Link Established
      s += sf(" handle %u", r.handle());
      r.hex("Link parameters", 8);
      r.u32("Access address", "0x%08x");
      break;
    case 0x11:  // Scan Status
      r.with("Scan", 1, [](uint32_t v) {
        return sf("0x%02x (inquiry scan %s, page scan %s)", v, v & 1 ? "on" : "off", v & 2 ? "on" : "off");
      });
      break;
    case 0x16:  // Activate Deactivate Traces Complete
    case 0x19: {  // Write BD Data Complete
      const uint8_t st = r.status();
      if (st) s += " " + status_str(st);
      break;
    }
    case 0x17: {  // LMP PDU Trace: the PDU, then the clock (and an ID) for LMP; a count for LL
      static const char* const types[] = {"RX LMP", "TX LMP", "ACK LMP", "RX LL", "TX LL", "ACK LL"};
      const uint8_t type = static_cast<uint8_t>(r.named("Type", 1, [](uint32_t v) { return v < 6 ? types[v] : "?"; }));
      s += std::string(" ") + (type < 6 ? types[type] : "?");
      s += sf(" handle %u", r.handle());
      const size_t left = r.left();
      if (type == 0 || type == 1) {  // a byte btmon does not name, the PDU, the clock, TX's ID
        r.hex("Unknown", 1);
        r.hex("PDU", left >= 5u + type ? left - 5 - type : 0);
        r.u32("Clock", "0x%08x");
        if (type == 1) r.u8("ID", "0x%02x");
      } else if (type == 2) {
        r.u32("Clock", "0x%08x");
        r.u8("ID", "0x%02x");
      } else if (type == 3 || type == 4) {
        r.u16("Count", "0x%04x");
        if (type == 4) {
          r.u8("ID", "0x%02x");
        } else {
          r.hex("Unknown", 1);
        }
        r.hex("Unknown", 2);
        r.hex("PDU", r.left());
      } else if (type == 5) {
        r.u16("Count", "0x%04x");
        r.u8("ID", "0x%02x");
      }
      break;
    }
    case 0x25: {  // SCO Rejected via LMP
      s += " " + r.addr();
      const uint8_t why = r.status("Reason");
      s += " " + status_str(why);
      break;
    }
    case 0x26:  // PTT Switch Notification: which packet table the link now uses
      s += sf(" handle %u", r.handle());
      s += r.named("Packet type table", 1, [](uint32_t v) { return v == 0 ? "basic rate" : v == 1 ? "enhanced data rate" : "?"; })
               ? " EDR" : " BR";
      break;
    case 0x29:  // System Exception
      r.named("Type", 1, [](uint32_t v) {
        static const char* const t[] = {"no exception", "undefined instruction", "prefetch abort", "data abort"};
        return v < 4 ? t[v] : "?";
      });
      r.hex("Exception data", r.left());
      break;
    case 0x2c:  // FW Trace String
      if (r.left() && mostly_text(r.cur(), r.left())) {
        const std::string text = printable(r.cur(), r.left(), 255);
        if (r.o.detail) r.o.add("Text", text, r.cur(), r.left());
        s += " " + quoted(r.cur(), r.left());
        r.skip(r.left());
      }
      break;
    default:
      break;
  }
  return s;
}

void hci_command(Out& o, const uint8_t* d, size_t len) {
  if (len < 3) {
    o.summary = "Command (truncated)";
    return;
  }
  const uint16_t op = le16(d);
  const uint8_t* p = d + 3;
  const size_t n = std::min<size_t>(d[2], len - 3);
  const std::string name = cmd_label(op, o.manufacturer);

  OPEN("HCI Command", name, d, len);
  F("Opcode", sf("0x%04x (OGF 0x%02x, OCF 0x%03x) %s", op, op >> 10, op & 0x3ff, name.c_str()), d, 2);
  F("Parameter length", std::to_string(d[2]), d + 2, 1);
  o.mark(p);

  std::string s = name;
  auto handle_field = [&](size_t at) {
    if (n >= at + 2) {
      const uint16_t h = le16(p + at) & 0x0fff;
      F("Handle", std::to_string(h), p + at, 2);
      return sf(" handle %u", h);
    }
    return std::string();
  };
  Rd r(o, p, n);
  if (intel_vendor(o.manufacturer, op)) {
    s += intel_cmd(r, op);
    rest(o, p, n);
    CLOSE();
    o.summary = s;
    return;
  }

  switch (op) {
    case 0x0401:  // Inquiry
    case 0x0403: {  // Periodic Inquiry Mode
      if (op == 0x0403) {
        r.time("Max period", 2, 1280);
        r.time("Min period", 2, 1280);
      }
      r.u24("LAP", "0x%06x");
      const uint8_t l = r.u8("Length", "%u (x 1.28 s)");
      r.u8("Responses", "%u (0: unlimited)");
      if (n >= 4) s += sf(" %.2f s", l * 1.28);
      break;
    }
    case 0x0405:  // Create Connection
      s += " " + r.addr();
      r.flags("Packet type", 2, kAclPacketTypes, 16);
      r.u8("Page scan repetition mode", "R%u");
      r.skip(1);
      r.with("Clock offset", 2, [](uint32_t v) { return sf("0x%04x%s", v & 0x7fff, v & 0x8000 ? " (valid)" : ""); });
      r.yes_no("Allow role switch", "yes", "no");
      if (n < 6) s = name;
      break;
    case 0x0419:  // Remote Name Request
      s += " " + r.addr();
      r.u8("Page scan repetition mode", "R%u");
      r.skip(1);
      r.with("Clock offset", 2, [](uint32_t v) { return sf("0x%04x%s", v & 0x7fff, v & 0x8000 ? " (valid)" : ""); });
      if (n < 6) s = name;
      break;
    case 0x040b:  // Link Key Request Reply
      s += " " + r.addr();
      r.hex("Link key", 16);
      if (n < 6) s = name;
      break;
    case 0x040d:  // PIN Code Request Reply
      s += " " + r.addr();
      r.u8("PIN length");
      r.hex("PIN", 16);
      if (n < 6) s = name;
      break;
    case 0x0409:  // Accept Connection Request
    case 0x040a:  // Reject Connection Request
    case 0x040c:
    case 0x040e:
    case 0x042b:
    case 0x042c:
    case 0x042d:
    case 0x042e:
    case 0x042f:
    case 0x0433:
    case 0x0434:
    case 0x080b:  // Switch Role
    case 0x0408:
    case 0x041a:
      if (n >= 6) {
        F("Address", bdaddr_str(p), p, 6);
        s += " " + bdaddr_str(p);
      }
      if ((op == 0x040a || op == 0x0434) && n >= 7) {
        F("Reason", status_str(p[6]), p + 6, 1);
      } else if ((op == 0x0409 || op == 0x080b) && n >= 7) {
        F("Role", p[6] ? "peripheral" : "central", p + 6, 1);
        s += p[6] ? " (peripheral)" : " (central)";
      } else if (op == 0x042b && n >= 9) {
        F("IO capability", sf("0x%02x (%s)", p[6], io_cap_name(p[6])), p + 6, 1);
        F("OOB data", sf("0x%02x (%s)", p[7], oob_present_name(p[7])), p + 7, 1);
        F("Authentication", sf("0x%02x (%s)", p[8], auth_req_name(p[8])), p + 8, 1);
        s += std::string(" ") + io_cap_name(p[6]);
      } else if (op == 0x042e && n >= 10) {
        F("Passkey", sf("%06u", le32(p + 6)), p + 6, 4);
      }
      break;
    case 0x0406:  // Disconnect
      s += handle_field(0);
      if (n >= 3) {
        F("Reason", status_str(p[2]), p + 2, 1);
        s += sf(" reason 0x%02x", p[2]);
      }
      break;
    case 0x040f:  // Change Connection Packet Type
      s += sf(" handle %u", r.handle());
      r.flags("Packet type", 2, kAclPacketTypes, 16);
      break;
    case 0x0413:  // Set Connection Encryption
      s += handle_field(0);
      if (n >= 3) {
        F("Encryption", p[2] ? "on" : "off", p + 2, 1);
        s += p[2] ? " on" : " off";
      }
      break;
    case 0x041c:  // Read Remote Extended Features
      s += sf(" handle %u", r.handle());
      s += sf(" page %u", r.u8("Page"));
      break;
    case 0x0428:  // Setup Synchronous Connection
    case 0x0429: {  // Accept Synchronous Connection Request
      if (op == 0x0428) s += sf(" handle %u", r.handle());
      else s += " " + r.addr();
      r.u32("TX bandwidth", "%u bytes/s");
      r.u32("RX bandwidth", "%u bytes/s");
      r.with("Max latency", 2, [](uint32_t v) { return v == 0xffff ? std::string("don't care") : sf("%u ms", v); });
      const uint16_t vs = static_cast<uint16_t>(r.with("Voice setting", 2, voice_setting_str));
      r.named("Retransmission effort", 1, retrans_effort_name);
      r.flags("Packet type", 2, kSyncPacketTypes, 10);
      if (n >= 14) s += (vs & 3) == 3 ? " transparent" : " CVSD";
      break;
    }
    case 0x042a:  // Reject Synchronous Connection Request
      s += " " + r.addr();
      r.status("Reason");
      break;
    case 0x043d:    // Enhanced Setup Synchronous Connection
    case 0x043e: {  // Enhanced Accept Synchronous Connection Request
      if (op == 0x043d) s += sf(" handle %u", r.handle());
      else s += " " + r.addr();
      r.u32("TX bandwidth", "%u bytes/s");
      r.u32("RX bandwidth", "%u bytes/s");
      const uint8_t air = r.has(5) ? r.cur()[0] : 0;
      r.with("TX coding format", 5, [&](uint32_t) { return codec_id_str(r.cur()); });
      r.with("RX coding format", 5, [&](uint32_t) { return codec_id_str(r.cur()); });
      r.u16("TX codec frame size", "%u bytes");
      r.u16("RX codec frame size", "%u bytes");
      r.u32("Input bandwidth", "%u bytes/s");
      r.u32("Output bandwidth", "%u bytes/s");
      r.with("Input coding format", 5, [&](uint32_t) { return codec_id_str(r.cur()); });
      r.with("Output coding format", 5, [&](uint32_t) { return codec_id_str(r.cur()); });
      r.u16("Input coded data size", "%u bits");
      r.u16("Output coded data size", "%u bits");
      static const char* const pcm[] = {"n/a", "1's complement", "2's complement", "sign-magnitude", "unsigned"};
      r.with("Input PCM data format", 1, [](uint32_t v) { return std::string(v < 5 ? pcm[v] : "?"); });
      r.with("Output PCM data format", 1, [](uint32_t v) { return std::string(v < 5 ? pcm[v] : "?"); });
      r.u8("Input PCM sample payload MSB position");
      r.u8("Output PCM sample payload MSB position");
      r.with("Input data path", 1, [](uint32_t v) { return sf("0x%02x (%s)", v, v ? "vendor" : "HCI"); });
      r.with("Output data path", 1, [](uint32_t v) { return sf("0x%02x (%s)", v, v ? "vendor" : "HCI"); });
      r.u8("Input transport unit size", "%u bits");
      r.u8("Output transport unit size", "%u bits");
      r.with("Max latency", 2, [](uint32_t v) { return v == 0xffff ? std::string("don't care") : sf("%u ms", v); });
      r.flags("Packet type", 2, kSyncPacketTypes, 10);
      r.named("Retransmission effort", 1, retrans_effort_name);
      if (n >= 15) s += std::string(" ") + coding_format_name(air);
      break;
    }
    case 0x0801:  // Hold Mode
      s += sf(" handle %u", r.handle());
      r.time("Max interval", 2, 0.625);
      r.time("Min interval", 2, 0.625);
      break;
    case 0x0803:  // Sniff Mode
      s += handle_field(0);
      if (n >= 10) {
        F("Max interval", sf("%u slots (%.2f ms)", le16(p + 2), le16(p + 2) * 0.625), p + 2, 2);
        F("Min interval", sf("%u slots (%.2f ms)", le16(p + 4), le16(p + 4) * 0.625), p + 4, 2);
        F("Attempt", std::to_string(le16(p + 6)), p + 6, 2);
        F("Timeout", std::to_string(le16(p + 8)), p + 8, 2);
        s += sf(" interval %.1f-%.1f ms", le16(p + 4) * 0.625, le16(p + 2) * 0.625);
      }
      break;
    case 0x080d:  // Write Link Policy Settings
      s += handle_field(0);
      if (n >= 4) F("Policy", sf("0x%04x (%s)", le16(p + 2), bit_names(le16(p + 2), kLinkPolicy, 4).c_str()), p + 2, 2);
      break;
    case 0x080f:  // Write Default Link Policy Settings
      r.flags("Policy", 2, kLinkPolicy, 4);
      break;
    case 0x0811:  // Sniff Subrating
      s += sf(" handle %u", r.handle());
      r.time("Max latency", 2, 0.625);
      r.time("Min remote timeout", 2, 0.625);
      r.time("Min local timeout", 2, 0.625);
      break;
    case 0x0c01:  // Set Event Mask
    case 0x0c63:  // Set Event Mask Page 2
      r.hex("Mask", 8);
      break;
    case 0x0c0a:  // Write PIN Type
      r.with("PIN type", 1, [](uint32_t v) { return std::string(v ? "fixed" : "variable"); });
      break;
    case 0x0c0d:  // Read Stored Link Key
    case 0x0c12:  // Delete Stored Link Key
      r.addr();
      r.yes_no(op == 0x0c0d ? "Read all" : "Delete all", "yes", "no");
      break;
    case 0x0c13:  // Write Local Name
      if (n) {
        const std::string nm = printable(p, n, 64);
        F("Name", "\"" + nm + "\"", p, n);
        s += " \"" + nm + "\"";
      }
      break;
    case 0x0c16:  // Write Connection Accept Timeout
    case 0x0c18:  // Write Page Timeout
      r.time("Timeout", 2, 0.625);
      break;
    case 0x0c1a:  // Write Scan Enable
      if (n >= 1) {
        static const char* modes[] = {"off", "inquiry scan", "page scan", "inquiry + page scan"};
        const char* m = p[0] < 4 ? modes[p[0]] : "?";
        F("Scan enable", m, p, 1);
        s += std::string(" ") + m;
      }
      break;
    case 0x0c1c:  // Write Page Scan Activity
    case 0x0c1e:  // Write Inquiry Scan Activity
      r.time("Interval", 2, 0.625);
      r.time("Window", 2, 0.625);
      break;
    case 0x0c20:  // Write Authentication Enable
      r.yes_no("Authentication");
      break;
    case 0x0c24:  // Write Class of Device
      if (n >= 3) {
        F("Class", cod_str(le24(p)), p, 3);
        s += " " + cod_str(le24(p));
      }
      break;
    case 0x0c26:  // Write Voice Setting
      r.with("Voice setting", 2, voice_setting_str);
      break;
    case 0x0c28:  // Write Automatic Flush Timeout
      s += sf(" handle %u", r.handle());
      r.with("Timeout", 2, [](uint32_t v) { return v ? sf("%.2f ms (0x%04x)", v * 0.625, v) : std::string("infinite"); });
      break;
    case 0x0c2d:  // Read Transmit Power Level
      s += sf(" handle %u", r.handle());
      r.with("Type", 1, [](uint32_t v) { return std::string(v ? "maximum" : "current"); });
      break;
    case 0x0c31:  // Set Controller To Host Flow Control
      r.with("Flow control", 1, [](uint32_t v) {
        static const char* const m[] = {"off", "ACL on, SCO off", "ACL off, SCO on", "ACL and SCO on"};
        return std::string(v < 4 ? m[v] : "?");
      });
      break;
    case 0x0c33:  // Host Buffer Size
      r.u16("ACL MTU");
      r.u8("SCO MTU");
      r.u16("ACL packets");
      r.u16("SCO packets");
      break;
    case 0x0c35: {  // Host Number of Completed Packets
      const uint8_t k = r.u8("Handles");
      for (unsigned i = 0; i < k && r.has(4); ++i) {
        const uint8_t* q = r.cur();
        F(sf("Handle %u", le16(q) & 0x0fff), sf("%u packets", le16(q + 2)), q, 4);
        r.skip(4);
      }
      break;
    }
    case 0x0c37:  // Write Link Supervision Timeout
      s += sf(" handle %u", r.handle());
      r.time("Timeout", 2, 0.625);
      break;
    case 0x0c3a: {  // Write Current IAC LAP
      const uint8_t k = r.u8("Number of IACs");
      for (unsigned i = 0; i < k && r.has(3); ++i) r.u24("IAC LAP", "0x%06x");
      break;
    }
    case 0x0c43:  // Write Inquiry Scan Type
    case 0x0c47:  // Write Page Scan Type
      r.with("Type", 1, [](uint32_t v) { return std::string(v ? "interlaced" : "standard"); });
      break;
    case 0x0c45: {  // Write Inquiry Mode
      const uint8_t m = static_cast<uint8_t>(r.named("Mode", 1, inquiry_mode_name));
      if (n) s += std::string(" ") + inquiry_mode_name(m);
      break;
    }
    case 0x0c52:  // Write Extended Inquiry Response
      if (n >= 1) {
        F("FEC required", p[0] ? "yes" : "no", p, 1);
        OPEN("EIR", "", p + 1, n - 1);
        const std::string name = ad_data(o, p + 1, n - 1);
        CLOSE();
        if (!name.empty()) s += " \"" + name + "\"";
      }
      break;
    case 0x0c59:  // Write Inquiry Transmit Power Level
      r.dbm("TX power");
      break;
    case 0x0c5b:  // Write Default Erroneous Data Reporting
      r.yes_no("Erroneous data reporting");
      break;
    case 0x0c5f:  // Enhanced Flush
      s += sf(" handle %u", r.handle());
      r.u8("Packet type", "%u (automatically flushable only)");
      break;
    case 0x0c6d:  // Write LE Host Supported
      r.yes_no("LE supported (host)");
      r.u8("Simultaneous LE and BR/EDR", "%u");
      if (n) s += p[0] ? " on" : " off";
      break;
    case 0x0c7c:  // Write Authenticated Payload Timeout
      s += sf(" handle %u", r.handle());
      r.time("Timeout", 2, 10);
      break;
    case 0x0c2f:  // Write Synchronous Flow Control Enable
    case 0x0c56:  // Write Simple Pairing Mode
    case 0x0c7a:
    case 0x200a:  // LE Set Advertising Enable
    case 0x202d:  // LE Set Address Resolution Enable
      if (n >= 1) {
        F("Enable", p[0] ? "yes" : "no", p, 1);
        s += p[0] ? " on" : " off";
      }
      break;
    case 0x1004:  // Read Local Extended Features
      s += sf(" page %u", r.u8("Page"));
      break;
    case 0x100e:    // Read Local Supported Codec Capabilities
    case 0x100f: {  // Read Local Supported Controller Delay
      if (r.has(5)) s += " " + codec_id_str(r.cur());
      r.with("Codec", 5, [&](uint32_t) { return codec_id_str(r.cur()); });
      static const char* const tr[] = {"BR/EDR ACL", "BR/EDR SCO", "LE CIS", "LE BIS"};
      r.with("Transport", 1, [](uint32_t v) { return sf("0x%02x (%s)", v, bit_names(v, tr, 4).c_str()); });
      r.with("Direction", 1, [](uint32_t v) { return std::string(v ? "output (controller to host)" : "input (host to controller)"); });
      if (op == 0x100f) r.hex("Codec configuration", r.u8("Codec configuration length"));
      break;
    }
    case 0x2001:  // LE Set Event Mask
      if (r.has(8)) {
        uint64_t m = 0;
        for (int i = 7; i >= 0; --i) m = m << 8 | p[i];
        std::string ev;
        for (unsigned b = 0; b < 64; ++b) {
          if (!(m >> b & 1)) continue;
          const char* nm = le_sub_name(static_cast<uint8_t>(b + 1));
          ev += (ev.empty() ? "" : ", ") + (nm ? std::string(nm + 3) : sf("0x%02x", b + 1));
        }
        F("Mask", hexbytes(p, 8) + " (" + (ev.empty() ? "none" : ev) + ")", p, 8);
      }
      break;
    case 0x2006:  // LE Set Advertising Parameters
      r.time("Min interval", 2, 0.625);
      r.time("Max interval", 2, 0.625);
      s += std::string(" ") + adv_type_name(r.named("Type", 1, adv_type_name));
      r.own_atype();
      r.atype("Peer address type");
      r.addr("Peer address");
      r.flags("Channel map", 1, kAdvChannels, 3);
      r.named("Filter policy", 1, adv_filter_name);
      if (n < 5) s = name;
      break;
    case 0x2008:    // LE Set Advertising Data
    case 0x2009: {  // LE Set Scan Response Data
      const uint8_t l = r.u8("Data length");
      if (r.has(1)) {
        adv_data_block(o, r.cur(), std::min<size_t>(l, r.left()), &s);
        r.skip(r.left());  // the rest of the 31 bytes is padding
      }
      break;
    }
    case 0x200c:  // LE Set Scan Enable
      if (n >= 2) {
        F("Enable", p[0] ? "yes" : "no", p, 1);
        F("Filter duplicates", p[1] ? "yes" : "no", p + 1, 1);
        s += p[0] ? " on" : " off";
      }
      break;
    case 0x2042:  // LE Set Extended Scan Enable
      if (n >= 6) {
        F("Enable", p[0] ? "yes" : "no", p, 1);
        static const char* const dup[] = {"no", "yes", "yes, reset each period"};
        F("Filter duplicates", sf("%u (%s)", p[1], p[1] < 3 ? dup[p[1]] : "?"), p + 1, 1);
        F("Duration", le16(p + 2) ? sf("%u ms", le16(p + 2) * 10) : std::string("until disabled"), p + 2, 2);
        F("Period", le16(p + 4) ? sf("%.2f s", le16(p + 4) * 1.28) : std::string("none"), p + 4, 2);
        s += p[0] ? " on" : " off";
      }
      break;
    case 0x2039:  // LE Set Extended Advertising Enable
      if (n >= 2) {
        F("Enable", p[0] ? "yes" : "no", p, 1);
        F("Number of sets", p[1] ? std::to_string(p[1]) : std::string("0 (all)"), p + 1, 1);
        s += p[0] ? " on" : " off";
        for (size_t i = 0; i < p[1] && 2 + 4 * (i + 1) <= n; ++i) {
          const uint8_t* q = p + 2 + 4 * i;
          F(sf("Set %zu", i), sf("handle %u, duration %s, max events %u", q[0],
                                 le16(q + 1) ? sf("%u ms", le16(q + 1) * 10).c_str() : "none", q[3]), q, 4);
          if (i < 4) s += sf("%s set %u", i ? "," : "", q[0]);
        }
      }
      break;
    case 0x200b:  // LE Set Scan Parameters
      if (n >= 7) {
        F("Type", p[0] ? "active" : "passive", p, 1);
        F("Interval", sf("%.2f ms", le16(p + 1) * 0.625), p + 1, 2);
        F("Window", sf("%.2f ms", le16(p + 3) * 0.625), p + 3, 2);
        F("Own address type", own_addr_type_name(p[5]), p + 5, 1);
        F("Filter policy", sf("0x%02x (%s)", p[6], scan_filter_name(p[6])), p + 6, 1);
        s += p[0] ? " active" : " passive";
      }
      break;
    case 0x2005:  // LE Set Random Address
      if (n >= 6) {
        F("Address", bdaddr_str(p), p, 6);
        s += " " + bdaddr_str(p);
      }
      break;
    case 0x2011:  // LE Add/Remove Device To/From Filter Accept List
    case 0x2012:
    case 0x2028:  // LE Remove Device From Resolving List
    case 0x2047:  // LE Add/Remove Device To/From Periodic Advertiser List
    case 0x2048:
      r.atype();
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr();
      if (op >= 0x2047) r.u8("SID");
      break;
    case 0x2027:  // LE Add Device To Resolving List
      r.atype();
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr();
      r.hex("Peer IRK", 16);
      r.hex("Local IRK", 16);
      break;
    case 0x200d:  // LE Create Connection
      if (n >= 25) {
        F("Scan interval", sf("%.2f ms", le16(p) * 0.625), p, 2);
        F("Scan window", sf("%.2f ms", le16(p + 2) * 0.625), p + 2, 2);
        F("Filter policy", p[4] ? "accept list" : "peer address", p + 4, 1);
        F("Peer address type", addr_type_name(p[5]), p + 5, 1);
        F("Peer address", bdaddr_str(p + 6), p + 6, 6);
        F("Own address type", own_addr_type_name(p[12]), p + 12, 1);
        F("Interval", sf("%.2f-%.2f ms", le16(p + 13) * 1.25, le16(p + 15) * 1.25), p + 13, 4);
        F("Latency", std::to_string(le16(p + 17)), p + 17, 2);
        F("Supervision timeout", sf("%u ms", le16(p + 19) * 10), p + 19, 2);
        F("CE length", sf("%.2f-%.2f ms", le16(p + 21) * 0.625, le16(p + 23) * 0.625), p + 21, 4);
        s += p[4] ? " (accept list)" : " " + bdaddr_str(p + 6);
      }
      break;
    case 0x2043:    // LE Extended Create Connection
    case 0x2085: {  // LE Extended Create Connection v2
      if (op == 0x2085) {
        r.with("Advertising handle", 1, [](uint32_t v) { return v == 0xff ? std::string("none (0xff)") : std::to_string(v); });
        r.with("Subevent", 1, [](uint32_t v) { return v == 0xff ? std::string("none (0xff)") : std::to_string(v); });
      }
      const uint8_t* at = r.cur();
      const uint8_t filter = static_cast<uint8_t>(r.with("Filter policy", 1, [](uint32_t v) {
        return std::string(v ? "accept list" : "peer address");
      }));
      r.own_atype();
      r.atype("Peer address type");
      const std::string peer = r.addr("Peer address");
      const uint8_t phys = r.phys("Initiating PHYs");
      for (unsigned b = 0; b < 3; ++b)
        if (phys >> b & 1) ext_conn_phy(r, kPhyBits[b]);
      if (at + 10 <= p + n) s += filter ? " (accept list)" : " " + peer;
      break;
    }
    case 0x2013:  // LE Connection Update
      s += handle_field(0);
      if (n >= 10) {
        F("Interval", sf("%.2f-%.2f ms", le16(p + 2) * 1.25, le16(p + 4) * 1.25), p + 2, 4);
        F("Latency", std::to_string(le16(p + 6)), p + 6, 2);
        F("Supervision timeout", sf("%u ms", le16(p + 8) * 10), p + 8, 2);
        s += sf(" interval %.2f-%.2f ms", le16(p + 2) * 1.25, le16(p + 4) * 1.25);
      }
      if (n >= 14) F("CE length", sf("%.2f-%.2f ms", le16(p + 10) * 0.625, le16(p + 12) * 0.625), p + 10, 4);
      break;
    case 0x2014:  // LE Set Host Channel Classification
      r.hex("Channel map", 5);
      break;
    case 0x2017:  // LE Encrypt
      r.hex("Key", 16);
      r.hex("Plaintext", 16);
      break;
    case 0x2019:  // LE Enable Encryption
      s += handle_field(0);
      if (n >= 28) {
        F("Random", hexbytes(p + 2, 8), p + 2, 8);
        F("EDIV", sf("0x%04x", le16(p + 10)), p + 10, 2);
        F("LTK", hexbytes(p + 12, 16), p + 12, 16);
      }
      break;
    case 0x201a:  // LE Long Term Key Request Reply
      s += sf(" handle %u", r.handle());
      r.hex("LTK", 16);
      break;
    case 0x2020:  // LE Remote Connection Parameter Request Reply
      s += sf(" handle %u", r.handle());
      r.time("Min interval", 2, 1.25);
      r.time("Max interval", 2, 1.25);
      r.u16("Latency");
      r.time("Supervision timeout", 2, 10);
      r.time("Min CE length", 2, 0.625);
      r.time("Max CE length", 2, 0.625);
      break;
    case 0x2021:  // LE Remote Connection Parameter Request Negative Reply
      s += sf(" handle %u", r.handle());
      r.status("Reason");
      break;
    case 0x2022:  // LE Set Data Length
      s += handle_field(0);
      if (n >= 6) {
        F("TX octets", std::to_string(le16(p + 2)), p + 2, 2);
        F("TX time", sf("%u us", le16(p + 4)), p + 4, 2);
        s += sf(" %u octets", le16(p + 2));
      }
      break;
    case 0x2024:  // LE Write Suggested Default Data Length
      s += sf(" %u octets", r.u16("TX octets"));
      r.u16("TX time", "%u us");
      break;
    case 0x2026:  // LE Generate DHKey
    case 0x205e:
      r.hex("Remote P-256 public key", 64);
      if (op == 0x205e) r.u8("Key type", "%u");
      break;
    case 0x202e:  // LE Set Resolvable Private Address Timeout
      s += sf(" %u s", r.u16("Timeout", "%u s"));
      break;
    case 0x2031:  // LE Set Default PHY
    case 0x2032: {  // LE Set PHY
      if (op == 0x2032) s += sf(" handle %u", r.handle());
      static const char* const all[] = {"no TX preference", "no RX preference"};
      r.flags("All PHYs", 1, all, 2);
      r.phys("TX PHYs");
      r.phys("RX PHYs");
      if (op == 0x2032) {
        r.with("PHY options", 2, [](uint32_t v) {
          static const char* const o2[] = {"no preference", "S=2", "S=8", "?"};
          return sf("0x%04x (coded %s)", v, o2[v & 3]);
        });
      }
      break;
    }
    case 0x2035:  // LE Set Advertising Set Random Address
      s += sf(" set %u", r.u8("Advertising handle"));
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr();
      break;
    case 0x2036:    // LE Set Extended Advertising Parameters
    case 0x207f: {  // LE Set Extended Advertising Parameters v2
      const uint8_t h = r.u8("Advertising handle");
      const uint16_t props = static_cast<uint16_t>(r.flags("Properties", 2, kAdvProps, 7));
      const uint32_t mn = r.time("Min interval", 3, 0.625);
      const uint32_t mx = r.time("Max interval", 3, 0.625);
      r.flags("Channel map", 1, kAdvChannels, 3);
      r.own_atype();
      r.atype("Peer address type");
      r.addr("Peer address");
      r.named("Filter policy", 1, adv_filter_name);
      r.with("TX power", 1, [](uint32_t v) { return v == 0x7f ? std::string("no preference (0x7f)") : sf("%d dBm", static_cast<int8_t>(v)); });
      r.phy("Primary PHY");
      r.u8("Secondary max skip");
      r.phy("Secondary PHY");
      r.u8("SID");
      r.yes_no("Scan request notifications");
      if (op == 0x207f) {
        r.with("Primary PHY options", 1, [](uint32_t v) { return sf("0x%02x", v); });
        r.with("Secondary PHY options", 1, [](uint32_t v) { return sf("0x%02x", v); });
      }
      if (n >= 9) {
        s += sf(" set %u %.2f-%.2f ms", h, mn * 0.625, mx * 0.625);
        s += props & 0x10 ? " legacy" : props & 1 ? " connectable" : props & 2 ? " scannable" : "";
      }
      break;
    }
    case 0x2037:    // LE Set Extended Advertising Data
    case 0x2038:    // LE Set Extended Scan Response Data
    case 0x203f: {  // LE Set Periodic Advertising Data
      const uint8_t h = r.u8("Advertising handle");
      r.named("Operation", 1, adv_data_op_name);
      if (op != 0x203f) {
        r.with("Fragment preference", 1, [](uint32_t v) { return std::string(v ? "minimize fragmentation" : "may fragment"); });
      }
      const uint8_t l = r.u8("Data length");
      if (n >= 1) s += sf(" set %u", h);
      if (r.has(1)) {
        adv_data_block(o, r.cur(), std::min<size_t>(l, r.left()), &s);
        r.skip(l);
      }
      break;
    }
    case 0x203c:  // LE Remove Advertising Set
      s += sf(" set %u", r.u8("Advertising handle"));
      break;
    case 0x203e:    // LE Set Periodic Advertising Parameters
    case 0x2086: {  // v2
      const uint8_t h = r.u8("Advertising handle");
      const uint16_t mn = static_cast<uint16_t>(r.time("Min interval", 2, 1.25));
      const uint16_t mx = static_cast<uint16_t>(r.time("Max interval", 2, 1.25));
      static const char* const props[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "include TX power"};
      r.flags("Properties", 2, props, 7);
      if (op == 0x2086) {
        r.u8("Subevents");
        r.time("Subevent interval", 1, 1.25);
        r.time("Response slot delay", 1, 1.25);
        r.time("Response slot spacing", 1, 0.125);
        r.u8("Response slots");
      }
      if (n >= 5) s += sf(" set %u %.2f-%.2f ms", h, mn * 1.25, mx * 1.25);
      break;
    }
    case 0x2040: {  // LE Set Periodic Advertising Enable
      static const char* const en[] = {"enabled", "include ADI"};
      const uint8_t e = static_cast<uint8_t>(r.flags("Enable", 1, en, 2));
      const uint8_t h = r.u8("Advertising handle");
      if (n >= 2) s += sf("%s set %u", e & 1 ? " on" : " off", h);
      break;
    }
    case 0x2041: {  // LE Set Extended Scan Parameters
      r.own_atype();
      r.named("Filter policy", 1, scan_filter_name);
      const uint8_t phys = r.phys("Scanning PHYs");
      for (unsigned b = 0; b < 3; ++b) {
        if (!(phys >> b & 1)) continue;
        OPEN_R(r, kPhyBits[b], "", r.cur(), std::min<size_t>(5, r.left()));
        const uint8_t type = r.yes_no("Type", "active", "passive");
        r.time("Interval", 2, 0.625);
        r.time("Window", 2, 0.625);
        CLOSE_R(r);
        if (s == name) s += type ? " active" : " passive";
      }
      break;
    }
    case 0x2044: {  // LE Periodic Advertising Create Sync
      const uint8_t opt = static_cast<uint8_t>(r.flags("Options", 1, kPaSyncOptions, 3));
      const uint8_t sid = r.u8("SID");
      r.atype();
      const std::string a = r.addr();
      r.u16("Skip");
      r.time("Sync timeout", 2, 10);
      r.flags("Sync CTE type", 1, kCteTypes, 5);
      if (n >= 9) s += opt & 1 ? " (periodic advertiser list)" : sf(" %s SID %u", a.c_str(), sid);
      break;
    }
    case 0x2046:  // LE Periodic Advertising Terminate Sync
      s += sf(" sync handle %u", r.handle("Sync handle"));
      break;
    case 0x204e:  // LE Set Privacy Mode
      r.atype("Peer address type");
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr("Peer address");
      r.named("Mode", 1, privacy_mode_name);
      break;
    case 0x2059:  // LE Set Periodic Advertising Receive Enable
      s += sf(" sync handle %u", r.handle("Sync handle"));
      r.flags("Enable", 1, kPaReceiveEnable, 2);
      break;
    case 0x205a:  // LE Periodic Advertising Sync Transfer
    case 0x205b:  // LE Periodic Advertising Set Info Transfer
      s += sf(" handle %u", r.handle());
      r.u16("Service data", "0x%04x");
      if (op == 0x205a) s += sf(" sync handle %u", r.handle("Sync handle"));
      else s += sf(" set %u", r.u8("Advertising handle"));
      break;
    case 0x205c:  // LE Set Periodic Advertising Sync Transfer Parameters
    case 0x205d:
      if (op == 0x205c) s += sf(" handle %u", r.handle());
      r.named("Mode", 1, past_mode_name);
      r.u16("Skip");
      r.time("Sync timeout", 2, 10);
      r.flags("CTE type", 1, kCteTypes, 5);
      break;
    case 0x2061:  // LE Read ISO TX Sync
      s += sf(" handle %u", r.handle());
      break;
    case 0x2062:    // LE Set CIG Parameters
    case 0x2063: {  // LE Set CIG Parameters Test
      const bool test = op == 0x2063;
      const uint8_t cig = r.u8("CIG ID");
      r.u24("SDU interval C to P", "%u us");
      r.u24("SDU interval P to C", "%u us");
      if (test) {
        r.u8("FT C to P");
        r.u8("FT P to C");
        r.time("ISO interval", 2, 1.25);
      }
      r.named("Worst case SCA", 1, sca_name);
      r.with("Packing", 1, [](uint32_t v) { return std::string(v ? "interleaved" : "sequential"); });
      r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "framed" : "unframed"); });
      if (!test) {
        r.u16("Max transport latency C to P", "%u ms");
        r.u16("Max transport latency P to C", "%u ms");
      }
      const uint8_t k = r.u8("CIS count");
      const size_t each = test ? 14 : 9;
      for (unsigned i = 0; i < k && r.has(each); ++i) {
        OPEN_R(r, sf("CIS %u", r.cur()[0]), "", r.cur(), each);
        r.u8("CIS ID");
        if (test) r.u8("NSE");
        r.u16("Max SDU C to P");
        r.u16("Max SDU P to C");
        if (test) {
          r.u16("Max PDU C to P");
          r.u16("Max PDU P to C");
        }
        r.phys("PHY C to P");
        r.phys("PHY P to C");
        if (test) {
          r.u8("BN C to P");
          r.u8("BN P to C");
        } else {
          r.u8("RTN C to P");
          r.u8("RTN P to C");
        }
        CLOSE_R(r);
      }
      if (n >= 1) s += sf(" CIG %u, %u CIS", cig, k);
      break;
    }
    case 0x2064: {  // LE Create CIS
      const uint8_t k = r.u8("CIS count");
      for (unsigned i = 0; i < k && r.has(4); ++i) {
        const uint8_t* q = r.cur();
        F(sf("CIS %u", i), sf("CIS handle %u, ACL handle %u", le16(q) & 0x0fff, le16(q + 2) & 0x0fff), q, 4);
        if (i < 3) s += sf("%s CIS %u ACL %u", i ? "," : "", le16(q) & 0x0fff, le16(q + 2) & 0x0fff);
        r.skip(4);
      }
      if (k > 3) s += sf(" (+%u more)", k - 3);
      break;
    }
    case 0x2065:  // LE Remove CIG
      s += sf(" CIG %u", r.u8("CIG ID"));
      break;
    case 0x2067:  // LE Reject CIS Request
      s += sf(" handle %u", r.handle());
      r.status("Reason");
      break;
    case 0x2068:    // LE Create BIG
    case 0x2069: {  // LE Create BIG Test
      const bool test = op == 0x2069;
      const uint8_t big = r.u8("BIG handle");
      const uint8_t adv = r.u8("Advertising handle");
      const uint8_t k = r.u8("Number of BIS");
      r.u24("SDU interval", "%u us");
      if (test) {
        r.time("ISO interval", 2, 1.25);
        r.u8("NSE");
      }
      r.u16("Max SDU");
      if (test) r.u16("Max PDU");
      else r.u16("Max transport latency", "%u ms");
      if (!test) r.u8("RTN");
      r.phys("PHY");
      r.with("Packing", 1, [](uint32_t v) { return std::string(v ? "interleaved" : "sequential"); });
      r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "framed" : "unframed"); });
      if (test) {
        r.u8("BN");
        r.u8("IRC");
        r.u8("PTO");
      }
      const uint8_t enc = r.yes_no("Encryption", "yes", "no");
      r.hex("Broadcast code", 16);
      if (n >= 3) s += sf(" BIG %u adv set %u, %u BIS%s", big, adv, k, enc ? ", encrypted" : "");
      break;
    }
    case 0x206a:  // LE Terminate BIG
      s += sf(" BIG %u", r.u8("BIG handle"));
      r.status("Reason");
      break;
    case 0x206b: {  // LE BIG Create Sync
      const uint8_t big = r.u8("BIG handle");
      const uint16_t sh = r.handle("Sync handle");
      const uint8_t enc = r.yes_no("Encryption", "yes", "no");
      r.hex("Broadcast code", 16);
      r.with("Max subevents", 1, [](uint32_t v) { return v ? std::to_string(v) : std::string("any (0)"); });
      r.time("Sync timeout", 2, 10);
      const uint8_t k = r.u8("Number of BIS");
      std::string bis;
      for (unsigned i = 0; i < k && r.has(1); ++i) bis += sf("%s%u", i ? "," : "", r.u8("BIS index"));
      if (n >= 3) s += sf(" BIG %u sync handle %u", big, sh) + (bis.empty() ? "" : " BIS " + bis) + (enc ? ", encrypted" : "");
      break;
    }
    case 0x206c:  // LE BIG Terminate Sync
      s += sf(" BIG %u", r.u8("BIG handle"));
      break;
    case 0x206d:  // LE Request Peer SCA
      s += sf(" handle %u", r.handle());
      break;
    case 0x206e: {  // LE Setup ISO Data Path
      s += sf(" handle %u", r.handle());
      const uint8_t dir = r.with("Direction", 1, [](uint32_t v) {
        return std::string(v ? "output (controller to host)" : "input (host to controller)");
      });
      const uint8_t path = static_cast<uint8_t>(r.named("Data path", 1, data_path_name));
      if (r.has(5)) {
        if (n >= 4) s += sf(" %s %s %s", dir ? "output" : "input", data_path_name(path), coding_format_name(r.cur()[0]));
      }
      r.with("Codec", 5, [&](uint32_t) { return codec_id_str(r.cur()); });
      r.u24("Controller delay", "%u us");
      r.hex("Codec configuration", r.u8("Codec configuration length"));
      break;
    }
    case 0x206f: {  // LE Remove ISO Data Path
      s += sf(" handle %u", r.handle());
      static const char* const dirs[] = {"input", "output"};
      r.flags("Direction", 1, dirs, 2);
      break;
    }
    case 0x2074:    // LE Set Host Feature
    case 0x2097: {  // v2
      const uint16_t bit = static_cast<uint16_t>(r.with("Bit number", op == 0x2097 ? 2 : 1, [](uint32_t v) {
        return sf("%u (%s)", v, v < kLeFeatureCount ? kLeFeatures[v] : "?");
      }));
      const uint8_t v = r.yes_no("Bit value", "on", "off");
      if (n >= 2) s += sf(" bit %u (%s) %s", bit, bit < kLeFeatureCount ? kLeFeatures[bit] : "?", v ? "on" : "off");
      break;
    }
    case 0x2076:  // LE Enhanced Read Transmit Power Level
    case 0x2077:  // LE Read Remote Transmit Power Level
      s += sf(" handle %u", r.handle());
      r.with("PHY", 1, [](uint32_t v) {
        static const char* const ph[] = {"?", "LE 1M", "LE 2M", "LE Coded S=8", "LE Coded S=2"};
        return sf("0x%02x (%s)", v, v < 5 ? ph[v] : "?");
      });
      break;
    case 0x2078:  // LE Set Path Loss Reporting Parameters
      s += sf(" handle %u", r.handle());
      r.u8("High threshold", "%u dB");
      r.u8("High hysteresis", "%u dB");
      r.u8("Low threshold", "%u dB");
      r.u8("Low hysteresis", "%u dB");
      r.u16("Min time spent", "%u connection events");
      break;
    case 0x2079:  // LE Set Path Loss Reporting Enable
      s += sf(" handle %u", r.handle());
      r.yes_no("Enable");
      break;
    case 0x207a:  // LE Set Transmit Power Reporting Enable
      s += sf(" handle %u", r.handle());
      r.yes_no("Local");
      r.yes_no("Remote");
      break;
    case 0x207c:  // LE Set Data Related Address Changes
      s += sf(" set %u", r.u8("Advertising handle"));
      r.flags("Change reasons", 1, kAddrChangeReasons, 2);
      break;
    case 0x207d:  // LE Set Default Subrate
    case 0x207e:  // LE Subrate Request
      if (op == 0x207e) s += sf(" handle %u", r.handle());
      r.u16("Min subrate factor");
      r.u16("Max subrate factor");
      r.u16("Max peripheral latency");
      r.u16("Continuation number");
      r.time("Supervision timeout", 2, 10);
      break;
    default:
      if (hci_cmd_has_handle(op)) s += handle_field(0);
      break;
  }
  rest(o, p, n);
  CLOSE();
  o.summary = s;
}

// ---------------------------------------------------------------------------------------------
// HCI events

// The codecs of Read Local Supported Codecs (v1: IDs; v2: each with the transports it runs over).
void local_codecs(Rd& rd, bool v2) {
  static const char* const tr[] = {"BR/EDR ACL", "BR/EDR SCO", "LE CIS", "LE BIS"};
  auto transports = [](uint32_t v) { return sf("0x%02x (%s)", v, bit_names(v, tr, 4).c_str()); };
  const uint8_t k = rd.u8("Supported codecs");
  for (unsigned i = 0; i < k && rd.has(v2 ? 2 : 1); ++i) {
    const uint8_t* q = rd.cur();
    if (rd.o.detail) {
      rd.o.add("Codec", sf("%s (0x%02x)", coding_format_name(q[0]), q[0]) + (v2 ? ", " + transports(q[1]) : ""), q, v2 ? 2 : 1);
    }
    rd.skip(v2 ? 2 : 1);
  }
  const uint8_t kv = rd.u8("Vendor codecs");
  for (unsigned i = 0; i < kv && rd.has(v2 ? 5 : 4); ++i) {
    const uint8_t* q = rd.cur();
    if (rd.o.detail) {
      const char* c = company_name(le16(q));
      rd.o.add("Vendor codec", sf("0x%04x:0x%04x", le16(q), le16(q + 2)) + (c ? std::string(" (") + c + ")" : "") +
                                   (v2 ? ", " + transports(q[4]) : ""), q, v2 ? 5 : 4);
    }
    rd.skip(v2 ? 5 : 4);
  }
}

// The return parameters of a Command Complete: every field of the commands people read the answer
// of, the handle of the per-connection ones.
std::string cmd_return(Out& o, uint16_t op, const uint8_t* r, size_t n) {
  if (n < 1) return {};
  F("Status", status_str(r[0]), r, 1);
  std::string s = r[0] ? sf(" status 0x%02x (%s)", r[0], error_name(r[0])) : "";
  // A failed command returns only the status, or zeroes after it: nothing worth decoding.
  if (r[0] != 0) return s;
  o.mark(r + 1);
  Rd rd(o, r + 1, n - 1);
  if (intel_vendor(o.manufacturer, op)) {
    s += intel_return(rd, op, n);
    rest(o, r + 1, n - 1, "Return parameters");
    return s;
  }
  auto inquiry_scan_type = [](uint32_t v) { return std::string(v ? "interlaced" : "standard"); };
  switch (op) {
    case 0x1001:  // Read Local Version Information
      if (n >= 9) {
        F("HCI version", sf("%u (Bluetooth %s, revision 0x%04x)", r[1], bt_version_name(r[1]), le16(r + 2)), r + 1, 3);
        F("LMP version", sf("%u (Bluetooth %s)", r[4], bt_version_name(r[4])), r + 4, 1);
        F("Manufacturer", company_str(le16(r + 5)), r + 5, 2);
        F("LMP subversion", sf("0x%04x", le16(r + 7)), r + 7, 2);
        s += sf(" HCI %u, manufacturer %u", r[1], le16(r + 5));
      }
      break;
    case 0x1002:  // Read Local Supported Commands
      if (n >= 65) {
        unsigned bits = 0;
        for (size_t i = 1; i < 65; ++i) bits += static_cast<unsigned>(__builtin_popcount(r[i]));
        F("Commands", sf("%u supported: ", bits) + hexbytes(r + 1, 64, 64), r + 1, 64);
      }
      break;
    case 0x1003:  // Read Local Supported Features
      if (n >= 9) F("Features", hexbytes(r + 1, 8) + " (" + lmp_features_str(r + 1, 0) + ")", r + 1, 8);
      break;
    case 0x1004:  // Read Local Extended Features
      if (n >= 11) {
        F("Page", sf("%u of %u", r[1], r[2]), r + 1, 2);
        F("Features", hexbytes(r + 3, 8) + " (" + lmp_features_str(r + 3, r[1]) + ")", r + 3, 8);
        s += sf(" page %u", r[1]);
      }
      break;
    case 0x1009:
      if (n >= 7) {
        F("Address", bdaddr_str(r + 1), r + 1, 6);
        s += " " + bdaddr_str(r + 1);
      }
      break;
    case 0x100b:  // Read Local Supported Codecs
    case 0x100d:  // v2
      local_codecs(rd, op == 0x100d);
      break;
    case 0x100c:  // Read Local Simple Pairing Options
      {
        static const char* const spo[] = {"remote public key validation"};
        rd.flags("Options", 1, spo, 1);
      }
      rd.u8("Max encryption key size");
      break;
    case 0x100e: {  // Read Local Supported Codec Capabilities
      const uint8_t k = rd.u8("Codec capabilities");
      for (unsigned i = 0; i < k && rd.has(1); ++i) {
        const uint8_t l = rd.raw(1);
        rd.hex("Capability", 1 + l);
      }
      break;
    }
    case 0x100f:  // Read Local Supported Controller Delay
      rd.u24("Min controller delay", "%u us");
      rd.u24("Max controller delay", "%u us");
      break;
    case 0x0c14:
      if (n >= 2) {
        const std::string nm = printable(r + 1, n - 1, 64);
        F("Name", "\"" + nm + "\"", r + 1, n - 1);
        s += " \"" + nm + "\"";
      }
      break;
    case 0x0c09:  // Read PIN Type
      rd.with("PIN type", 1, [](uint32_t v) { return std::string(v ? "fixed" : "variable"); });
      break;
    case 0x0c0d:  // Read Stored Link Key
      rd.u16("Max keys");
      rd.u16("Keys read");
      break;
    case 0x0c12:  // Delete Stored Link Key
      rd.u16("Keys deleted");
      break;
    case 0x0c15:  // Read Connection Accept Timeout
    case 0x0c17:  // Read Page Timeout
      rd.time("Timeout", 2, 0.625);
      break;
    case 0x0c19:  // Read Scan Enable
      rd.with("Scan enable", 1, [](uint32_t v) {
        static const char* modes[] = {"off", "inquiry scan", "page scan", "inquiry + page scan"};
        return std::string(v < 4 ? modes[v] : "?");
      });
      break;
    case 0x0c1b:  // Read Page Scan Activity
    case 0x0c1d:  // Read Inquiry Scan Activity
      rd.time("Interval", 2, 0.625);
      rd.time("Window", 2, 0.625);
      break;
    case 0x0c1f:  // Read Authentication Enable
      rd.yes_no("Authentication");
      break;
    case 0x0c23:  // Read Class of Device
      if (n >= 4) {
        F("Class", cod_str(le24(r + 1)), r + 1, 3);
        s += " " + cod_str(le24(r + 1));
      }
      break;
    case 0x0c25:  // Read Voice Setting
      rd.with("Voice setting", 2, voice_setting_str);
      break;
    case 0x0c27:  // Read Automatic Flush Timeout
      s += sf(" handle %u", rd.handle());
      rd.with("Timeout", 2, [](uint32_t v) { return v ? sf("%.2f ms (0x%04x)", v * 0.625, v) : std::string("infinite"); });
      break;
    case 0x0c2d:  // Read Transmit Power Level
      s += sf(" handle %u", rd.handle());
      if (rd.has(1)) s += sf(" %d dBm", static_cast<int8_t>(rd.cur()[0]));
      rd.dbm("TX power");
      break;
    case 0x0c2e:  // Read Synchronous Flow Control Enable
    case 0x0c55:  // Read Simple Pairing Mode
    case 0x0c5a:  // Read Default Erroneous Data Reporting
    case 0x0c79:  // Read Secure Connections Host Support
      rd.yes_no("Enable");
      break;
    case 0x0c36:  // Read Link Supervision Timeout
      s += sf(" handle %u", rd.handle());
      rd.time("Timeout", 2, 0.625);
      break;
    case 0x0c38:  // Read Number of Supported IAC
      rd.u8("Supported IACs");
      break;
    case 0x0c39: {  // Read Current IAC LAP
      const uint8_t k = rd.u8("Number of IACs");
      for (unsigned i = 0; i < k && rd.has(3); ++i) rd.u24("IAC LAP", "0x%06x");
      break;
    }
    case 0x0c42:  // Read Inquiry Scan Type
    case 0x0c46:  // Read Page Scan Type
      rd.with("Type", 1, inquiry_scan_type);
      break;
    case 0x0c44:  // Read Inquiry Mode
      rd.named("Mode", 1, inquiry_mode_name);
      break;
    case 0x0c51:  // Read Extended Inquiry Response
      if (rd.has(1)) {
        rd.yes_no("FEC required", "yes", "no");
        OPEN("EIR", "", rd.cur(), rd.left());
        const std::string nm = ad_data(o, rd.cur(), rd.left());
        CLOSE();
        if (!nm.empty()) s += " \"" + nm + "\"";
      }
      break;
    case 0x0c58:  // Read Inquiry Response TX Power Level
      rd.dbm("TX power");
      break;
    case 0x0c66:  // Read Flow Control Mode
      rd.with("Mode", 1, [](uint32_t v) { return std::string(v ? "data block based" : "packet based"); });
      break;
    case 0x0c6c:  // Read LE Host Supported
      rd.yes_no("LE supported (host)");
      rd.u8("Simultaneous LE and BR/EDR");
      break;
    case 0x0c7b:  // Read Authenticated Payload Timeout
      s += sf(" handle %u", rd.handle());
      rd.time("Timeout", 2, 10);
      break;
    case 0x0c57:  // Read Local OOB Data
      rd.hex("Hash C", 16);
      rd.hex("Randomizer R", 16);
      break;
    case 0x0c7d:  // Read Local OOB Extended Data
      rd.hex("Hash C (P-192)", 16);
      rd.hex("Randomizer R (P-192)", 16);
      rd.hex("Hash C (P-256)", 16);
      rd.hex("Randomizer R (P-256)", 16);
      break;
    case 0x0809:  // Role Discovery
      s += sf(" handle %u", rd.handle());
      if (rd.has(1)) s += rd.cur()[0] ? " peripheral" : " central";
      rd.with("Role", 1, [](uint32_t v) { return std::string(v ? "peripheral" : "central"); });
      break;
    case 0x080c:  // Read Link Policy Settings
      s += sf(" handle %u", rd.handle());
      rd.flags("Policy", 2, kLinkPolicy, 4);
      break;
    case 0x080e:  // Read Default Link Policy Settings
      rd.flags("Policy", 2, kLinkPolicy, 4);
      break;
    case 0x1005:
      if (n >= 8) {
        F("ACL MTU", std::to_string(le16(r + 1)), r + 1, 2);
        F("SCO MTU", std::to_string(r[3]), r + 3, 1);
        F("ACL packets", std::to_string(le16(r + 4)), r + 4, 2);
        F("SCO packets", std::to_string(le16(r + 6)), r + 6, 2);
        s += sf(" ACL %u x %u, SCO %u x %u", le16(r + 1), le16(r + 4), r[3], le16(r + 6));
      }
      break;
    case 0x2002:
    case 0x2060:
      if (n >= 4) {
        F("LE MTU", std::to_string(le16(r + 1)), r + 1, 2);
        F("LE packets", std::to_string(r[3]), r + 3, 1);
        s += sf(" LE %u x %u", le16(r + 1), r[3]);
      }
      if (op == 0x2060 && n >= 7) {
        F("ISO MTU", std::to_string(le16(r + 4)), r + 4, 2);
        F("ISO packets", std::to_string(r[6]), r + 6, 1);
        s += sf(", ISO %u x %u", le16(r + 4), r[6]);
      }
      break;
    case 0x1401:  // Read Failed Contact Counter
      s += sf(" handle %u", rd.handle());
      rd.u16("Failed contact counter");
      break;
    case 0x1403:  // Read Link Quality
      s += sf(" handle %u", rd.handle());
      rd.u8("Link quality", "%u/255");
      break;
    case 0x1405:  // Read RSSI
      if (n >= 4) {
        F("Handle", std::to_string(le16(r + 1) & 0x0fff), r + 1, 2);
        F("RSSI", sf("%d dB", static_cast<int8_t>(r[3])), r + 3, 1);
        s += sf(" handle %u RSSI %d", le16(r + 1) & 0x0fff, static_cast<int8_t>(r[3]));
      }
      break;
    case 0x1406:  // Read AFH Channel Map
      s += sf(" handle %u", rd.handle());
      rd.yes_no("AFH mode");
      rd.hex("Channel map", 10);
      break;
    case 0x1407:  // Read Clock
      s += sf(" handle %u", rd.handle());
      rd.u32("Clock", "0x%08x");
      rd.with("Accuracy", 2, [](uint32_t v) { return v == 0xffff ? std::string("unknown") : sf("+/- %.4f ms", v * 0.3125); });
      break;
    case 0x1408:  // Read Encryption Key Size
      if (n >= 4) {
        F("Handle", std::to_string(le16(r + 1) & 0x0fff), r + 1, 2);
        F("Key size", std::to_string(r[3]), r + 3, 1);
        s += sf(" handle %u key size %u", le16(r + 1) & 0x0fff, r[3]);
      }
      break;
    case 0x2003:  // LE Read Local Supported Features
      if (n >= 9) F("Features", hexbytes(r + 1, 8) + " (" + le_features_str(r + 1) + ")", r + 1, 8);
      break;
    case 0x2007:  // LE Read Advertising Channel TX Power
      rd.dbm("TX power");
      break;
    case 0x200f:  // LE Read Filter Accept List Size
    case 0x202a:  // LE Read Resolving List Size
    case 0x204a:  // LE Read Periodic Advertiser List Size
      if (rd.has(1)) s += sf(" %u", rd.cur()[0]);
      rd.u8("List size");
      break;
    case 0x2015:  // LE Read Channel Map
      s += sf(" handle %u", rd.handle());
      rd.hex("Channel map", 5);
      break;
    case 0x2017:  // LE Encrypt
      rd.hex("Encrypted data", 16);
      break;
    case 0x2018:  // LE Rand
      rd.hex("Random number", 8);
      break;
    case 0x201c:  // LE Read Supported States
      rd.hex("States", 8);
      break;
    case 0x2023:  // LE Read Suggested Default Data Length
      if (rd.has(2)) s += sf(" %u octets", le16(rd.cur()));
      rd.u16("TX octets");
      rd.u16("TX time", "%u us");
      break;
    case 0x202f:  // LE Read Maximum Data Length
      rd.u16("Max TX octets");
      rd.u16("Max TX time", "%u us");
      rd.u16("Max RX octets");
      rd.u16("Max RX time", "%u us");
      break;
    case 0x2030: {  // LE Read PHY
      s += sf(" handle %u", rd.handle());
      const uint8_t tx = rd.phy("TX PHY");
      const uint8_t rx = rd.phy("RX PHY");
      if (n >= 5) s += sf(" TX %s RX %s", phy_name(tx), phy_name(rx));
      break;
    }
    case 0x2036:  // LE Set Extended Advertising Parameters
    case 0x207f:
      if (rd.has(1)) s += sf(" TX power %d dBm", static_cast<int8_t>(rd.cur()[0]));
      rd.dbm("Selected TX power");
      break;
    case 0x203a:  // LE Read Maximum Advertising Data Length
      rd.u16("Max data length");
      break;
    case 0x203b:  // LE Read Number of Supported Advertising Sets
      if (rd.has(1)) s += sf(" %u", rd.cur()[0]);
      rd.u8("Advertising sets");
      break;
    case 0x204b:  // LE Read Transmit Power
      rd.dbm("Min TX power");
      rd.dbm("Max TX power");
      break;
    case 0x204c:  // LE Read RF Path Compensation
      rd.with("TX path compensation", 2, [](uint32_t v) { return sf("%.1f dB", static_cast<int16_t>(v) / 10.0); });
      rd.with("RX path compensation", 2, [](uint32_t v) { return sf("%.1f dB", static_cast<int16_t>(v) / 10.0); });
      break;
    case 0x2061:  // LE Read ISO TX Sync
      s += sf(" handle %u", rd.handle());
      rd.u16("Sequence number");
      rd.u32("Timestamp", "%u us");
      rd.u24("Time offset", "%u us");
      break;
    case 0x2062:    // LE Set CIG Parameters
    case 0x2063: {  // LE Set CIG Parameters Test
      const uint8_t cig = rd.u8("CIG ID");
      const uint8_t k = rd.u8("CIS count");
      std::string hs;
      for (unsigned i = 0; i < k && rd.has(2); ++i) hs += sf("%s%u", i ? "," : "", rd.handle("CIS handle"));
      if (n >= 3) s += sf(" CIG %u", cig) + (hs.empty() ? "" : " CIS " + hs);
      break;
    }
    case 0x2065:  // LE Remove CIG
      s += sf(" CIG %u", rd.u8("CIG ID"));
      break;
    case 0x206c:  // LE BIG Terminate Sync
      s += sf(" BIG %u", rd.u8("BIG handle"));
      break;
    case 0x2072:  // LE ISO Read Test Counters
      s += sf(" handle %u", rd.handle());
      rd.u32("Received packets");
      rd.u32("Missed packets");
      rd.u32("Failed packets");
      break;
    case 0x2073:  // LE ISO Test End
      s += sf(" handle %u", rd.handle());
      rd.u32("Received packets");
      rd.u32("Missed packets");
      rd.u32("Failed packets");
      break;
    case 0x2075:  // LE Read ISO Link Quality
      s += sf(" handle %u", rd.handle());
      rd.u32("TX unacked packets");
      rd.u32("TX flushed packets");
      rd.u32("TX last subevent packets");
      rd.u32("Retransmitted packets");
      rd.u32("CRC error packets");
      rd.u32("RX unreceived packets");
      rd.u32("Duplicate packets");
      break;
    case 0x2076:  // LE Enhanced Read Transmit Power Level
      s += sf(" handle %u", rd.handle());
      rd.phy("PHY");
      if (rd.has(1)) s += sf(" %d dBm", static_cast<int8_t>(rd.cur()[0]));
      rd.dbm("Current TX power");
      rd.dbm("Max TX power");
      break;
    case 0x0408:  // Create Connection Cancel
    case 0x040b:  // Link Key Request Reply, the other replies about a device
    case 0x040c:
    case 0x040d:
    case 0x040e:
    case 0x041a:  // Remote Name Request Cancel
    case 0x042b:
    case 0x042c:
    case 0x042d:
    case 0x042e:
    case 0x042f:
    case 0x0430:
    case 0x0433:
    case 0x0434:
      if (rd.has(6)) s += " " + bdaddr_str(rd.cur());
      rd.addr();
      break;
    default:
      if ((hci_cmd_has_handle(op) || op == 0x2067 || op == 0x205a || op == 0x205b || op == 0x205c) && n >= 3) {
        F("Handle", std::to_string(le16(r + 1) & 0x0fff), r + 1, 2);
        s += sf(" handle %u", le16(r + 1) & 0x0fff);
      }
      break;
  }
  rest(o, r + 1, n - 1, "Return parameters");
  return s;
}

const char* mode_name(uint8_t m) {
  switch (m) {
    case 0: return "active";
    case 1: return "hold";
    case 2: return "sniff";
    case 3: return "park";
    default: return "?";
  }
}

const char* link_key_type_name(uint8_t t) {
  switch (t) {
    case 0x00: return "combination";
    case 0x01: return "local unit";
    case 0x02: return "remote unit";
    case 0x03: return "debug combination";
    case 0x04: return "unauthenticated combination P-192";
    case 0x05: return "authenticated combination P-192";
    case 0x06: return "changed combination";
    case 0x07: return "unauthenticated combination P-256";
    case 0x08: return "authenticated combination P-256";
    default: return "?";
  }
}

const char* air_mode_name(uint8_t m) {
  switch (m) {
    case 0x00: return "u-law";
    case 0x01: return "A-law";
    case 0x02: return "CVSD";
    case 0x03: return "transparent";
    default: return "?";
  }
}

const char* io_cap_name(uint8_t c) {
  switch (c) {
    case 0: return "DisplayOnly";
    case 1: return "DisplayYesNo";
    case 2: return "KeyboardOnly";
    case 3: return "NoInputNoOutput";
    case 4: return "KeyboardDisplay";
    default: return "?";
  }
}

// The central's sleep clock accuracy of LE (Enhanced) Connection Complete and PAST/PA sync events.
const char* clock_accuracy_name(uint32_t a) {
  static const char* const names[] = {"500 ppm", "250 ppm", "150 ppm", "100 ppm", "75 ppm", "50 ppm", "30 ppm", "20 ppm"};
  return a < 8 ? names[a] : "?";
}

// The Extended Advertising Report's event type: the PDU's properties and how complete the data is.
std::string ext_adv_type_str(uint16_t et) {
  static const char* const bits[] = {"connectable", "scannable", "directed", "scan response", "legacy"};
  static const char* const status[] = {"complete", "incomplete, more to come", "incomplete, truncated", "?"};
  return sf("0x%04x (%s; data %s)", et, bit_names(et & 0x1f, bits, 5).c_str(), status[(et >> 5) & 3]);
}

// The data status of a (periodic) advertising report: whether more fragments follow.
const char* data_status_name(uint32_t s) {
  switch (s) {
    case 0: return "complete";
    case 1: return "incomplete, more to come";
    case 2: return "incomplete, truncated";
    case 0xff: return "failed to receive";
    default: return "?";
  }
}

std::string le_meta(Out& o, const uint8_t* p, size_t n) {
  if (n < 1) return "LE Meta Event (truncated)";
  const uint8_t sub = p[0];
  const char* nm = le_sub_name(sub);
  std::string s = nm ? nm : sf("LE Meta 0x%02x", sub);
  F("Subevent", sf("0x%02x (%s)", sub, nm ? nm : "unknown"), p, 1);
  const uint8_t* q = p + 1;
  const size_t m = n - 1;
  o.mark(q);
  Rd r(o, q, m);

  switch (sub) {
    case 0x01:
    case 0x0a:
    case 0x29:
      if (m >= 11) {
        F("Status", status_str(q[0]), q, 1);
        F("Handle", std::to_string(le16(q + 1) & 0x0fff), q + 1, 2);
        F("Role", q[3] ? "peripheral" : "central", q + 3, 1);
        F("Peer address type", addr_type_name(q[4]), q + 4, 1);
        F("Peer address", bdaddr_str(q + 5), q + 5, 6);
        const size_t at = sub == 0x01 ? 11 : 23;  // enhanced: + local and peer RPA
        if (sub != 0x01 && m >= 23) {
          F("Local resolvable private address", bdaddr_str(q + 11), q + 11, 6);
          F("Peer resolvable private address", bdaddr_str(q + 17), q + 17, 6);
        }
        if (m >= at + 6) {
          F("Interval", sf("%.2f ms", le16(q + at) * 1.25), q + at, 2);
          F("Latency", std::to_string(le16(q + at + 2)), q + at + 2, 2);
          F("Supervision timeout", sf("%u ms", le16(q + at + 4) * 10), q + at + 4, 2);
        }
        if (m >= at + 7) F("Central clock accuracy", clock_accuracy_name(q[at + 6]), q + at + 6, 1);
        if (sub == 0x29 && m >= at + 10) {
          F("Advertising handle", q[at + 7] == 0xff ? std::string("none") : std::to_string(q[at + 7]), q + at + 7, 1);
          F("Sync handle", le16(q + at + 8) == 0xffff ? std::string("none") : std::to_string(le16(q + at + 8)), q + at + 8, 2);
        }
        if (q[0]) {
          s += " " + bdaddr_str(q + 5) + " failed: " + status_str(q[0]);
        } else {
          s += sf(" handle %u %s (%s) %s", le16(q + 1) & 0x0fff, bdaddr_str(q + 5).c_str(),
                  addr_type_name(q[4]), q[3] ? "peripheral" : "central");
          if (m >= at + 2) s += sf(" interval %.2f ms", le16(q + at) * 1.25);
        }
      }
      break;
    case 0x02: {  // Advertising Report: per report event type, address type, address, data, RSSI
      if (m < 1) break;
      F("Reports", std::to_string(q[0]), q, 1);
      static const char* types[] = {"ADV_IND", "ADV_DIRECT_IND", "ADV_SCAN_IND", "ADV_NONCONN_IND", "SCAN_RSP"};
      size_t off = 1;
      std::string first;
      for (size_t i = 0; i < q[0] && off + 9 <= m; ++i) {
        const uint8_t* a = q + off;
        const size_t dl = a[8];
        if (off + 9 + dl + 1 > m) break;
        const int8_t rssi = static_cast<int8_t>(a[9 + dl]);
        OPEN(sf("Report %zu", i), bdaddr_str(a + 2), a, 10 + dl);
        F("Event type", a[0] < 5 ? types[a[0]] : "?", a, 1);
        F("Address type", addr_type_name(a[1]), a + 1, 1);
        F("Address", bdaddr_str(a + 2), a + 2, 6);
        F("Data length", std::to_string(dl), a + 8, 1);
        const std::string name = ad_data(o, a + 9, dl);
        F("RSSI", sf("%d dBm", rssi), a + 9 + dl, 1);
        CLOSE();
        if (i == 0) {
          first = sf(" %s %s (%s) %d dBm", a[0] < 5 ? types[a[0]] : "?", bdaddr_str(a + 2).c_str(),
                     addr_type_name(a[1]), rssi);
          if (!name.empty()) first += " \"" + name + "\"";
        }
        off += 10 + dl;
      }
      s += first;
      if (q[0] > 1) s += sf(" (+%u more)", q[0] - 1);
      break;
    }
    case 0x0b: {  // Directed Advertising Report
      const uint8_t k = r.u8("Reports");
      for (unsigned i = 0; i < k && r.has(16); ++i) {
        const uint8_t* a = r.cur();
        OPEN_R(r, sf("Report %u", i), bdaddr_str(a + 2), a, 16);
        r.named("Event type", 1, adv_type_name);
        r.atype();
        r.addr();
        r.atype("Direct address type");
        r.addr("Direct address");
        r.with("RSSI", 1, [](uint32_t v) { return sf("%d dBm", static_cast<int8_t>(v)); });
        CLOSE_R(r);
        if (i == 0) s += sf(" %s (%s) %d dBm", bdaddr_str(a + 2).c_str(), addr_type_name(a[1]), static_cast<int8_t>(a[15]));
      }
      break;
    }
    case 0x0d: {  // Extended Advertising Report
      if (m < 1) break;
      F("Reports", std::to_string(q[0]), q, 1);
      size_t off = 1;
      std::string first;
      for (size_t i = 0; i < q[0] && off + 24 <= m; ++i) {
        const uint8_t* a = q + off;
        const size_t dl = a[23];
        if (off + 24 + dl > m) break;
        const uint16_t et = le16(a);
        const int8_t rssi = static_cast<int8_t>(a[13]);
        OPEN(sf("Report %zu", i), bdaddr_str(a + 3), a, 24 + dl);
        F("Event type", ext_adv_type_str(et), a, 2);
        F("Address type", a[2] == 0xff ? "anonymous" : addr_type_name(a[2]), a + 2, 1);
        F("Address", bdaddr_str(a + 3), a + 3, 6);
        F("Primary PHY", sf("0x%02x (%s)", a[9], phy_name(a[9])), a + 9, 1);
        F("Secondary PHY", a[10] ? sf("0x%02x (%s)", a[10], phy_name(a[10])) : std::string("none"), a + 10, 1);
        F("SID", a[11] == 0xff ? std::string("none (0xff)") : std::to_string(a[11]), a + 11, 1);
        F("TX power", a[12] == 0x7f ? std::string("not available") : sf("%d dBm", static_cast<int8_t>(a[12])), a + 12, 1);
        F("RSSI", rssi == 127 ? std::string("not available") : sf("%d dBm", rssi), a + 13, 1);
        F("Periodic advertising interval", le16(a + 14) ? sf("%.2f ms", le16(a + 14) * 1.25) : std::string("none"), a + 14, 2);
        if (et & 0x04) {
          F("Direct address type", addr_type_name(a[16]), a + 16, 1);
          F("Direct address", bdaddr_str(a + 17), a + 17, 6);
        } else {
          F("Direct address", "none", a + 16, 7);
        }
        F("Data length", std::to_string(dl), a + 23, 1);
        std::string info;
        const std::string name = ad_data(o, a + 24, dl, i == 0 ? &info : nullptr);
        CLOSE();
        if (i == 0) {
          first = sf(" %s (%s) %d dBm", bdaddr_str(a + 3).c_str(), addr_type_name(a[2]), rssi);
          if (!name.empty()) first += " \"" + name + "\"";
          if (!info.empty()) first += " " + info;
        }
        off += 24 + dl;
      }
      s += first;
      if (q[0] > 1) s += sf(" (+%u more)", q[0] - 1);
      break;
    }
    case 0x03:  // Connection Update Complete
      if (m >= 9) {
        F("Status", status_str(q[0]), q, 1);
        F("Handle", std::to_string(le16(q + 1) & 0x0fff), q + 1, 2);
        F("Interval", sf("%.2f ms", le16(q + 3) * 1.25), q + 3, 2);
        F("Latency", std::to_string(le16(q + 5)), q + 5, 2);
        F("Supervision timeout", sf("%u ms", le16(q + 7) * 10), q + 7, 2);
        s += sf(" handle %u", le16(q + 1) & 0x0fff);
        s += q[0] ? " " + status_str(q[0]) : sf(" interval %.2f ms latency %u timeout %u ms",
                                               le16(q + 3) * 1.25, le16(q + 5), le16(q + 7) * 10);
      }
      break;
    case 0x04:  // Read Remote Features Complete
      if (m >= 11) {
        F("Status", status_str(q[0]), q, 1);
        F("Handle", std::to_string(le16(q + 1) & 0x0fff), q + 1, 2);
        F("Features", hexbytes(q + 3, 8) + (q[0] ? "" : " (" + le_features_str(q + 3) + ")"), q + 3, 8);
        s += sf(" handle %u", le16(q + 1) & 0x0fff) + (q[0] ? " " + status_str(q[0]) : "");
      }
      break;
    case 0x05:  // Long Term Key Request
      if (m >= 12) {
        F("Handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("Random", hexbytes(q + 2, 8), q + 2, 8);
        F("EDIV", sf("0x%04x", le16(q + 10)), q + 10, 2);
        s += sf(" handle %u", le16(q) & 0x0fff);
      }
      break;
    case 0x06:  // Remote Connection Parameter Request
      if (m >= 10) {
        F("Handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("Interval", sf("%.2f-%.2f ms", le16(q + 2) * 1.25, le16(q + 4) * 1.25), q + 2, 4);
        F("Latency", std::to_string(le16(q + 6)), q + 6, 2);
        F("Supervision timeout", sf("%u ms", le16(q + 8) * 10), q + 8, 2);
        s += sf(" handle %u interval %.2f-%.2f ms", le16(q) & 0x0fff, le16(q + 2) * 1.25,
                le16(q + 4) * 1.25);
      }
      break;
    case 0x07:  // Data Length Change
      if (m >= 10) {
        F("Handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("Max TX", sf("%u octets, %u us", le16(q + 2), le16(q + 4)), q + 2, 4);
        F("Max RX", sf("%u octets, %u us", le16(q + 6), le16(q + 8)), q + 6, 4);
        s += sf(" handle %u TX %u RX %u octets", le16(q) & 0x0fff, le16(q + 2), le16(q + 6));
      }
      break;
    case 0x08:  // Read Local P-256 Public Key Complete
    case 0x09:  // Generate DHKey Complete
      if (r.status() && m >= 1) s += " " + status_str(q[0]);
      r.hex(sub == 0x08 ? "Public key" : "DHKey", sub == 0x08 ? 64 : 32);
      break;
    case 0x0c:  // PHY Update Complete
      if (m >= 5) {
        static const char* phys[] = {"?", "1M", "2M", "Coded"};
        F("Status", status_str(q[0]), q, 1);
        F("Handle", std::to_string(le16(q + 1) & 0x0fff), q + 1, 2);
        F("TX PHY", q[3] < 4 ? phys[q[3]] : "?", q + 3, 1);
        F("RX PHY", q[4] < 4 ? phys[q[4]] : "?", q + 4, 1);
        s += sf(" handle %u TX %s RX %s", le16(q + 1) & 0x0fff, q[3] < 4 ? phys[q[3]] : "?",
                q[4] < 4 ? phys[q[4]] : "?");
      }
      break;
    case 0x0e:    // Periodic Advertising Sync Established
    case 0x24: {  // v2
      const uint8_t st = r.status();
      const uint16_t sh = r.handle("Sync handle");
      const uint8_t sid = r.u8("SID");
      r.atype();
      const std::string a = r.addr();
      r.phy("PHY");
      const uint16_t iv = static_cast<uint16_t>(r.time("Interval", 2, 1.25));
      r.named("Clock accuracy", 1, clock_accuracy_name);
      if (sub == 0x24) {
        r.u8("Subevents");
        r.time("Subevent interval", 1, 1.25);
        r.time("Response slot delay", 1, 1.25);
        r.time("Response slot spacing", 1, 0.125);
      }
      if (m >= 1 && st) s += " " + a + " failed: " + status_str(st);
      else if (m >= 15) s += sf(" sync handle %u %s SID %u interval %.2f ms", sh, a.c_str(), sid, iv * 1.25);
      break;
    }
    case 0x0f:    // Periodic Advertising Report
    case 0x25: {  // v2
      const uint16_t sh = r.handle("Sync handle");
      r.dbm("TX power");
      const int8_t rssi = static_cast<int8_t>(r.with("RSSI", 1, [](uint32_t v) {
        return static_cast<int8_t>(v) == 127 ? std::string("not available") : sf("%d dBm", static_cast<int8_t>(v));
      }));
      static const char* const cte[] = {"AoA", "AoD 1 us", "AoD 2 us"};
      r.with("CTE type", 1, [](uint32_t v) { return v == 0xff ? std::string("none") : sf("0x%02x (%s)", v, v < 3 ? cte[v] : "?"); });
      if (sub == 0x25) {
        r.u16("Periodic event counter");
        r.with("Subevent", 1, [](uint32_t v) { return v == 0xff ? std::string("none") : std::to_string(v); });
      }
      const uint8_t ds = static_cast<uint8_t>(r.named("Data status", 1, data_status_name));
      const uint8_t dl = r.u8("Data length");
      std::string name, info;
      if (r.has(1) && dl) {
        // A fragment that more follow (data status 1) is cut mid-structure: only a complete one
        // is read for the summary.
        name = ad_data(o, r.cur(), std::min<size_t>(dl, r.left()), ds == 0 ? &info : nullptr);
        r.skip(dl);
      }
      if (m >= 7) {
        s += sf(" sync handle %u %d dBm, %u bytes", sh, rssi, dl);
        if (ds == 1 || ds == 2) s += sf(" (%s)", data_status_name(ds));
        if (!name.empty()) s += " \"" + name + "\"";
        if (!info.empty()) s += " " + info;
      }
      break;
    }
    case 0x10:  // Periodic Advertising Sync Lost
      s += sf(" sync handle %u", r.handle("Sync handle"));
      break;
    case 0x12:  // Advertising Set Terminated
      if (m >= 5) {
        F("Status", status_str(q[0]), q, 1);
        F("Advertising handle", std::to_string(q[1]), q + 1, 1);
        F("Connection handle", std::to_string(le16(q + 2) & 0x0fff), q + 2, 2);
        F("Completed events", std::to_string(q[4]), q + 4, 1);
        s += sf(" set %u", q[1]) + (q[0] ? " " + status_str(q[0]) : sf(" handle %u", le16(q + 2) & 0x0fff));
      }
      break;
    case 0x13:  // Scan Request Received
      s += sf(" set %u", r.u8("Advertising handle"));
      r.atype("Scanner address type");
      if (r.has(6)) s += " from " + bdaddr_str(r.cur());
      r.addr("Scanner address");
      break;
    case 0x14:  // Channel Selection Algorithm
      if (m >= 3) {
        F("Handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("Algorithm", sf("#%u", q[2] + 1), q + 2, 1);
        s += sf(" handle %u #%u", le16(q) & 0x0fff, q[2] + 1);
      }
      break;
    case 0x18:    // Periodic Advertising Sync Transfer Received
    case 0x26: {  // v2
      const uint8_t st = r.status();
      const uint16_t h = r.handle();
      r.u16("Service data", "0x%04x");
      const uint16_t sh = r.handle("Sync handle");
      const uint8_t sid = r.u8("SID");
      r.atype();
      const std::string a = r.addr();
      r.phy("PHY");
      r.time("Interval", 2, 1.25);
      r.named("Clock accuracy", 1, clock_accuracy_name);
      if (sub == 0x26) {
        r.u8("Subevents");
        r.time("Subevent interval", 1, 1.25);
        r.time("Response slot delay", 1, 1.25);
        r.time("Response slot spacing", 1, 0.125);
      }
      if (m >= 3) s += sf(" handle %u", h);
      if (m >= 1 && st) s += " " + status_str(st);
      else if (m >= 15) s += sf(" sync handle %u %s SID %u", sh, a.c_str(), sid);
      break;
    }
    case 0x19:    // CIS Established
    case 0x2a: {  // v2
      const uint8_t st = r.status();
      const uint16_t h = r.handle("CIS handle");
      r.u24("CIG sync delay", "%u us");
      r.u24("CIS sync delay", "%u us");
      const uint32_t lc = r.u24("Transport latency C to P", "%u us");
      const uint32_t lp = r.u24("Transport latency P to C", "%u us");
      r.phy("PHY C to P");
      r.phy("PHY P to C");
      r.u8("NSE");
      r.u8("BN C to P");
      r.u8("BN P to C");
      r.u8("FT C to P");
      r.u8("FT P to C");
      r.u16("Max PDU C to P");
      r.u16("Max PDU P to C");
      const uint16_t iv = static_cast<uint16_t>(r.time("ISO interval", 2, 1.25));
      if (sub == 0x2a) {
        r.u24("Sub interval", "%u us");
        r.u16("Max SDU C to P");
        r.u16("Max SDU P to C");
        r.u24("SDU interval C to P", "%u us");
        r.u24("SDU interval P to C", "%u us");
        r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "framed" : "unframed"); });
      }
      if (m >= 3) s += sf(" handle %u", h) + (st ? " " + status_str(st) : "");
      if (!st && m >= 28) s += sf(" ISO interval %.2f ms, latency %.1f/%.1f ms", iv * 1.25, lc / 1000.0, lp / 1000.0);
      break;
    }
    case 0x1a:
      if (m >= 6) {
        F("ACL handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("CIS handle", std::to_string(le16(q + 2) & 0x0fff), q + 2, 2);
        F("CIG", std::to_string(q[4]), q + 4, 1);
        F("CIS", std::to_string(q[5]), q + 5, 1);
        s += sf(" ACL %u CIS %u", le16(q) & 0x0fff, le16(q + 2) & 0x0fff);
      }
      break;
    case 0x1b: {  // Create BIG Complete
      const uint8_t st = r.status();
      const uint8_t big = r.u8("BIG handle");
      r.u24("BIG sync delay", "%u us");
      const uint32_t lat = r.u24("Transport latency", "%u us");
      r.phy("PHY");
      r.u8("NSE");
      r.u8("BN");
      r.u8("PTO");
      r.u8("IRC");
      r.u16("Max PDU");
      const uint16_t iv = static_cast<uint16_t>(r.time("ISO interval", 2, 1.25));
      const uint8_t k = r.u8("Number of BIS");
      std::string hs;
      for (unsigned i = 0; i < k && r.has(2); ++i) hs += sf("%s%u", i ? "," : "", r.handle("BIS handle"));
      if (m >= 1 && st) s += " " + status_str(st);
      else if (m >= 2) s += sf(" BIG %u", big) + (hs.empty() ? "" : " BIS " + hs) +
                            (m >= 18 ? sf(", ISO interval %.2f ms, latency %.1f ms", iv * 1.25, lat / 1000.0) : "");
      break;
    }
    case 0x1c:  // Terminate BIG Complete
    case 0x1e:  // BIG Sync Lost
      s += sf(" BIG %u", r.u8("BIG handle"));
      if (r.has(1)) s += sf(" reason 0x%02x (%s)", r.cur()[0], error_name(r.cur()[0]));
      r.status("Reason");
      break;
    case 0x1d: {  // BIG Sync Established
      const uint8_t st = r.status();
      const uint8_t big = r.u8("BIG handle");
      const uint32_t lat = r.u24("Transport latency", "%u us");
      r.u8("NSE");
      r.u8("BN");
      r.u8("PTO");
      r.u8("IRC");
      r.u16("Max PDU");
      const uint16_t iv = static_cast<uint16_t>(r.time("ISO interval", 2, 1.25));
      const uint8_t k = r.u8("Number of BIS");
      std::string hs;
      for (unsigned i = 0; i < k && r.has(2); ++i) hs += sf("%s%u", i ? "," : "", r.handle("BIS handle"));
      if (m >= 1 && st) s += " " + status_str(st);
      else if (m >= 2) s += sf(" BIG %u", big) + (hs.empty() ? "" : " BIS " + hs) +
                            (m >= 14 ? sf(", ISO interval %.2f ms, latency %.1f ms", iv * 1.25, lat / 1000.0) : "");
      break;
    }
    case 0x1f: {  // Request Peer SCA Complete
      const uint8_t st = r.status();
      s += sf(" handle %u", r.handle());
      if (st) s += " " + status_str(st);
      r.named("Peer clock accuracy", 1, sca_name);
      break;
    }
    case 0x20:  // Path Loss Threshold
      s += sf(" handle %u", r.handle());
      if (r.has(1)) s += sf(" %u dB", r.cur()[0]);
      r.with("Current path loss", 1, [](uint32_t v) { return v == 0xff ? std::string("unavailable") : sf("%u dB", v); });
      r.with("Zone entered", 1, [](uint32_t v) {
        static const char* const z[] = {"low", "middle", "high"};
        return std::string(v < 3 ? z[v] : "?");
      });
      break;
    case 0x21: {  // Transmit Power Reporting
      const uint8_t st = r.status();
      s += sf(" handle %u", r.handle());
      if (st) s += " " + status_str(st);
      r.with("Reason", 1, [](uint32_t v) {
        static const char* const why[] = {"local power changed", "remote power changed", "read remote completed"};
        return std::string(v < 3 ? why[v] : "?");
      });
      r.with("PHY", 1, [](uint32_t v) {
        static const char* const ph[] = {"?", "LE 1M", "LE 2M", "LE Coded S=8", "LE Coded S=2"};
        return std::string(v < 5 ? ph[v] : "?");
      });
      if (r.has(1)) s += sf(" %d dBm", static_cast<int8_t>(r.cur()[0]));
      r.with("TX power", 1, [](uint32_t v) {
        return v == 0x7e ? std::string("remote not managing power") : v == 0x7f ? std::string("not available")
                                                                                 : sf("%d dBm", static_cast<int8_t>(v));
      });
      static const char* const fl[] = {"at minimum", "at maximum"};
      r.flags("Flags", 1, fl, 2);
      r.with("Delta", 1, [](uint32_t v) { return v == 0x7f ? std::string("not available") : sf("%d dB", static_cast<int8_t>(v)); });
      break;
    }
    case 0x22: {  // BIGInfo Advertising Report
      const uint16_t sh = r.handle("Sync handle");
      const uint8_t k = r.u8("Number of BIS");
      r.u8("NSE");
      const uint16_t iv = static_cast<uint16_t>(r.time("ISO interval", 2, 1.25));
      r.u8("BN");
      r.u8("PTO");
      r.u8("IRC");
      r.u16("Max PDU");
      r.u24("SDU interval", "%u us");
      const uint16_t sdu = r.u16("Max SDU");
      r.phy("PHY");
      r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "framed" : "unframed"); });
      const uint8_t enc = r.yes_no("Encryption", "yes", "no");
      if (m >= 19) s += sf(" sync handle %u, %u BIS, ISO interval %.2f ms, max SDU %u%s", sh, k, iv * 1.25, sdu, enc ? ", encrypted" : "");
      break;
    }
    case 0x23: {  // Subrate Change
      const uint8_t st = r.status();
      s += sf(" handle %u", r.handle());
      if (st) s += " " + status_str(st);
      const uint16_t f = r.u16("Subrate factor");
      r.u16("Peripheral latency");
      r.u16("Continuation number");
      r.time("Supervision timeout", 2, 10);
      if (!st && m >= 5) s += sf(" factor %u", f);
      break;
    }
    case 0x11:  // Scan Timeout: no parameters
      break;
    default:
      break;
  }
  rest(o, q, m);
  return s;
}

void hci_event(Out& o, const uint8_t* d, size_t len) {
  if (len < 2) {
    o.summary = "Event (truncated)";
    return;
  }
  const uint8_t code = d[0];
  const uint8_t* p = d + 2;
  const size_t n = std::min<size_t>(d[1], len - 2);
  const char* nm = evt_name(code);
  std::string s = nm ? nm : sf("Event 0x%02x", code);

  OPEN("HCI Event", s, d, len);
  F("Event code", sf("0x%02x (%s)", code, nm ? nm : "unknown"), d, 1);
  F("Parameter length", std::to_string(d[1]), d + 1, 1);
  o.mark(p);
  Rd r(o, p, n);

  auto status_handle = [&]() {
    if (n >= 3) {
      F("Status", status_str(p[0]), p, 1);
      F("Handle", std::to_string(le16(p + 1) & 0x0fff), p + 1, 2);
      s += sf(" handle %u", le16(p + 1) & 0x0fff);
      if (p[0]) s += " " + status_str(p[0]);
    }
  };

  switch (code) {
    case 0x0e: {  // Command Complete
      if (n < 3) break;
      const uint16_t op = le16(p + 1);
      F("Num HCI command packets", std::to_string(p[0]), p, 1);
      F("Command opcode", sf("0x%04x (%s)", op, cmd_label(op, o.manufacturer).c_str()), p + 1, 2);
      s = cmd_label(op, o.manufacturer) + " complete" + cmd_return(o, op, p + 3, n - 3);
      break;
    }
    case 0x0f: {  // Command Status
      if (n < 4) break;
      const uint16_t op = le16(p + 2);
      F("Status", status_str(p[0]), p, 1);
      F("Num HCI command packets", std::to_string(p[1]), p + 1, 1);
      F("Command opcode", sf("0x%04x (%s)", op, cmd_label(op, o.manufacturer).c_str()), p + 2, 2);
      s = cmd_label(op, o.manufacturer) + (p[0] ? " status " + status_str(p[0]) : " pending");
      break;
    }
    case 0x13: {  // Number of Completed Packets
      if (n < 1) break;
      F("Handles", std::to_string(p[0]), p, 1);
      for (size_t i = 0; i < p[0] && 1 + 4 * (i + 1) <= n; ++i) {
        const uint8_t* q = p + 1 + 4 * i;
        F(sf("Handle %u", le16(q) & 0x0fff), sf("%u packets", le16(q + 2)), q, 4);
        s += sf("%s handle %u: %u", i ? "," : "", le16(q) & 0x0fff, le16(q + 2));
      }
      break;
    }
    case 0x03:  // Connection Complete
      if (n >= 11) {
        F("Status", status_str(p[0]), p, 1);
        F("Handle", std::to_string(le16(p + 1) & 0x0fff), p + 1, 2);
        F("Address", bdaddr_str(p + 3), p + 3, 6);
        F("Link type", p[9] == 1 ? "ACL" : p[9] == 0 ? "SCO" : "?", p + 9, 1);
        F("Encryption", p[10] ? "on" : "off", p + 10, 1);
        s += p[0] ? " " + bdaddr_str(p + 3) + " failed: " + status_str(p[0])
                  : sf(" handle %u %s (%s)", le16(p + 1) & 0x0fff, bdaddr_str(p + 3).c_str(),
                       p[9] == 1 ? "ACL" : "SCO");
      }
      break;
    case 0x04:  // Connection Request
      if (n >= 10) {
        F("Address", bdaddr_str(p), p, 6);
        F("Class", cod_str(le24(p + 6)), p + 6, 3);
        F("Link type", p[9] == 1 ? "ACL" : p[9] == 2 ? "eSCO" : "SCO", p + 9, 1);
        s += " " + bdaddr_str(p) + (p[9] == 1 ? " ACL" : p[9] == 2 ? " eSCO" : " SCO");
      }
      break;
    case 0x05:  // Disconnection Complete
      if (n >= 4) {
        F("Status", status_str(p[0]), p, 1);
        F("Handle", std::to_string(le16(p + 1) & 0x0fff), p + 1, 2);
        F("Reason", status_str(p[3]), p + 3, 1);
        s += sf(" handle %u", le16(p + 1) & 0x0fff) +
             (p[0] ? " " + status_str(p[0]) : sf(" reason 0x%02x (%s)", p[3], error_name(p[3])));
      }
      break;
    case 0x07:  // Remote Name Request Complete
      if (n >= 7) {
        F("Status", status_str(p[0]), p, 1);
        F("Address", bdaddr_str(p + 1), p + 1, 6);
        const std::string nm2 = printable(p + 7, n - 7, 64);
        if (n > 7) F("Name", "\"" + nm2 + "\"", p + 7, n - 7);
        s += " " + bdaddr_str(p + 1) + (p[0] ? " " + status_str(p[0]) : " \"" + nm2 + "\"");
      }
      break;
    case 0x08:  // Encryption Change
    case 0x59:
      status_handle();
      if (n >= 4) {
        static const char* enc[] = {"off", "on (E0 / AES-CCM)", "on (AES-CCM)"};
        F("Encryption", p[3] < 3 ? enc[p[3]] : "?", p + 3, 1);
        if (!p[0]) s += p[3] ? " on" : " off";
      }
      if (code == 0x59 && n >= 5) F("Key size", std::to_string(p[4]), p + 4, 1);
      break;
    case 0x0c:  // Read Remote Version Information Complete
      status_handle();
      if (n >= 8) {
        F("LMP version", sf("%u (Bluetooth %s)", p[3], bt_version_name(p[3])), p + 3, 1);
        F("Manufacturer", company_str(le16(p + 4)), p + 4, 2);
        F("LMP subversion", sf("0x%04x", le16(p + 6)), p + 6, 2);
        if (!p[0]) s += sf(" LMP %u manufacturer %u", p[3], le16(p + 4));
      }
      break;
    case 0x0b:  // Read Remote Supported Features Complete
      status_handle();
      if (n >= 11) F("Features", hexbytes(p + 3, 8) + (p[0] ? "" : " (" + lmp_features_str(p + 3, 0) + ")"), p + 3, 8);
      break;
    case 0x23:
      status_handle();
      if (n >= 13) {
        F("Page", sf("%u of %u", p[3], p[4]), p + 3, 2);
        F("Features", hexbytes(p + 5, 8) + (p[0] ? "" : " (" + lmp_features_str(p + 5, p[3]) + ")"), p + 5, 8);
        if (!p[0]) s += sf(" page %u", p[3]);
      }
      break;
    case 0x10:  // Hardware Error
      if (n >= 1) {
        F("Code", sf("0x%02x", p[0]), p, 1);
        s += sf(" 0x%02x", p[0]);
      }
      break;
    case 0x12:  // Role Change
      if (n >= 8) {
        F("Status", status_str(p[0]), p, 1);
        F("Address", bdaddr_str(p + 1), p + 1, 6);
        F("Role", p[7] ? "peripheral" : "central", p + 7, 1);
        s += " " + bdaddr_str(p + 1) + (p[0] ? " " + status_str(p[0]) : p[7] ? " peripheral" : " central");
      }
      break;
    case 0x14:  // Mode Change
      status_handle();
      if (n >= 6) {
        F("Mode", mode_name(p[3]), p + 3, 1);
        F("Interval", sf("%u slots (%.2f ms)", le16(p + 4), le16(p + 4) * 0.625), p + 4, 2);
        if (!p[0]) s += std::string(" ") + mode_name(p[3]) + (p[3] ? sf(" %.2f ms", le16(p + 4) * 0.625) : "");
      }
      break;
    case 0x16:  // PIN Code Request
    case 0x17:  // Link Key Request
    case 0x31:  // IO Capability Request
    case 0x34:  // User Passkey Request
    case 0x35:
      if (n >= 6) {
        F("Address", bdaddr_str(p), p, 6);
        s += " " + bdaddr_str(p);
      }
      break;
    case 0x18:  // Link Key Notification
      if (n >= 23) {
        F("Address", bdaddr_str(p), p, 6);
        F("Link key", hexbytes(p + 6, 16), p + 6, 16);
        F("Key type", sf("0x%02x (%s)", p[22], link_key_type_name(p[22])), p + 22, 1);
        s += " " + bdaddr_str(p) + sf(" type %u", p[22]);
      }
      break;
    case 0x32:  // IO Capability Response
      if (n >= 9) {
        F("Address", bdaddr_str(p), p, 6);
        F("IO capability", io_cap_name(p[6]), p + 6, 1);
        F("OOB data", sf("0x%02x (%s)", p[7], oob_present_name(p[7])), p + 7, 1);
        F("Authentication", sf("0x%02x (%s)", p[8], auth_req_name(p[8])), p + 8, 1);
        s += " " + bdaddr_str(p) + " " + io_cap_name(p[6]);
      }
      break;
    case 0x33:  // User Confirmation Request
    case 0x3b:  // User Passkey Notification
      if (n >= 10) {
        F("Address", bdaddr_str(p), p, 6);
        F("Passkey", sf("%06u", le32(p + 6)), p + 6, 4);
        s += " " + bdaddr_str(p) + sf(" %06u", le32(p + 6));
      }
      break;
    case 0x36:  // Simple Pairing Complete
    case 0x01:  // Inquiry Complete
      if (n >= 1) {
        F("Status", status_str(p[0]), p, 1);
        if (code == 0x36 && n >= 7) {
          F("Address", bdaddr_str(p + 1), p + 1, 6);
          s += " " + bdaddr_str(p + 1);
        }
        if (p[0]) s += " " + status_str(p[0]);
      }
      break;
    case 0x02:  // Inquiry Result
    case 0x22: {  // Inquiry Result with RSSI
      if (n < 1) break;
      F("Responses", std::to_string(p[0]), p, 1);
      for (size_t i = 0; i < p[0] && 1 + 14 * (i + 1) <= n; ++i) {
        const uint8_t* q = p + 1 + 14 * i;
        if (code == 0x22) {
          F(sf("Response %zu", i), sf("%s %s, RSSI %d dBm, R%u, clock offset 0x%04x", bdaddr_str(q).c_str(),
                                      cod_str(le24(q + 8)).c_str(), static_cast<int8_t>(q[13]), q[6], le16(q + 11) & 0x7fff), q, 14);
        } else {
          F(sf("Response %zu", i), sf("%s %s, R%u, clock offset 0x%04x", bdaddr_str(q).c_str(),
                                      cod_str(le24(q + 9)).c_str(), q[6], le16(q + 12) & 0x7fff), q, 14);
        }
        if (i == 0) s += " " + bdaddr_str(q);
      }
      if (p[0] > 1) s += sf(" (+%u more)", p[0] - 1);
      break;
    }
    case 0x2f:  // Extended Inquiry Result
      if (n >= 15) {
        F("Address", bdaddr_str(p + 1), p + 1, 6);
        F("Class", cod_str(le24(p + 9)), p + 9, 3);
        F("RSSI", sf("%d dBm", static_cast<int8_t>(p[14])), p + 14, 1);
        OPEN("EIR", "", p + 15, n - 15);
        const std::string name = ad_data(o, p + 15, n - 15);
        CLOSE();
        s += sf(" %s %d dBm", bdaddr_str(p + 1).c_str(), static_cast<int8_t>(p[14]));
        if (!name.empty()) s += " \"" + name + "\"";
      }
      break;
    case 0x2c:  // Synchronous Connection Complete
      if (n >= 17) {
        F("Status", status_str(p[0]), p, 1);
        F("Handle", std::to_string(le16(p + 1) & 0x0fff), p + 1, 2);
        F("Address", bdaddr_str(p + 3), p + 3, 6);
        F("Link type", p[9] == 2 ? "eSCO" : "SCO", p + 9, 1);
        F("TX interval", sf("%u slots", p[10]), p + 10, 1);
        F("Retransmission window", sf("%u slots", p[11]), p + 11, 1);
        F("RX packet length", std::to_string(le16(p + 12)), p + 12, 2);
        F("TX packet length", std::to_string(le16(p + 14)), p + 14, 2);
        F("Air mode", sf("0x%02x (%s)", p[16], air_mode_name(p[16])), p + 16, 1);
        s += p[0] ? " " + bdaddr_str(p + 3) + " failed: " + status_str(p[0])
                  : sf(" handle %u %s (%s)", le16(p + 1) & 0x0fff, bdaddr_str(p + 3).c_str(),
                       p[9] == 2 ? "eSCO" : "SCO");
        if (!p[0]) s += std::string(" ") + air_mode_name(p[16]);
      }
      break;
    case 0x0d:  // QoS Setup Complete
    case 0x21:  // Flow Specification Complete
      status_handle();
      r.skip(3);
      if (code == 0x0d) r.u8("Unused");
      else r.with("Flow direction", 1, [](uint32_t v) { return std::string(v ? "incoming" : "outgoing"); });
      r.with("Service type", 1, [](uint32_t v) {
        static const char* const t[] = {"no traffic", "best effort", "guaranteed"};
        return std::string(v < 3 ? t[v] : "?");
      });
      r.u32("Token rate", "%u bytes/s");
      if (code == 0x21) r.u32("Token bucket size", "%u bytes");
      r.u32("Peak bandwidth", "%u bytes/s");
      r.u32(code == 0x0d ? "Latency" : "Access latency", "%u us");
      if (code == 0x0d) r.u32("Delay variation", "%u us");
      break;
    case 0x15: {  // Return Link Keys
      const uint8_t k = r.u8("Keys");
      for (unsigned i = 0; i < k && r.has(22); ++i) {
        r.addr();
        r.hex("Link key", 16);
      }
      break;
    }
    case 0x1a:  // Data Buffer Overflow
      r.with("Link type", 1, [](uint32_t v) { return std::string(v == 1 ? "ACL" : v == 0 ? "SCO" : "?"); });
      break;
    case 0x1c:  // Read Clock Offset Complete
      status_handle();
      r.skip(3);
      r.with("Clock offset", 2, [](uint32_t v) { return sf("0x%04x", v & 0x7fff); });
      break;
    case 0x1d:  // Connection Packet Type Changed
      status_handle();
      r.skip(3);
      r.flags("Packet type", 2, kAclPacketTypes, 16);
      break;
    case 0x1e:  // QoS Violation
      s += sf(" handle %u", r.handle());
      break;
    case 0x20:  // Page Scan Repetition Mode Change
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr();
      r.u8("Page scan repetition mode", "R%u");
      break;
    case 0x2d:  // Synchronous Connection Changed
      status_handle();
      r.skip(3);
      r.u8("TX interval", "%u slots");
      r.u8("Retransmission window", "%u slots");
      r.u16("RX packet length");
      r.u16("TX packet length");
      break;
    case 0x2e:  // Sniff Subrating
      status_handle();
      r.skip(3);
      r.time("Max TX latency", 2, 0.625);
      r.time("Max RX latency", 2, 0.625);
      r.time("Min remote timeout", 2, 0.625);
      r.time("Min local timeout", 2, 0.625);
      break;
    case 0x3c:  // Keypress Notification
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr();
      r.with("Type", 1, [](uint32_t v) {
        static const char* const t[] = {"entry started", "digit entered", "digit erased", "cleared", "entry completed"};
        return std::string(v < 5 ? t[v] : "?");
      });
      break;
    case 0x3d:  // Remote Host Supported Features Notification
      if (r.has(6)) s += " " + bdaddr_str(r.cur());
      r.addr();
      if (r.has(8)) F("Features", hexbytes(r.cur(), 8) + " (" + lmp_features_str(r.cur(), 1) + ")", r.cur(), 8);
      break;
    case 0x11:  // Flush Occurred
    case 0x1b:  // Max Slots Change
    case 0x38:  // Link Supervision Timeout Changed
    case 0x39:
    case 0x57:
      if (n >= 2) {
        F("Handle", std::to_string(le16(p) & 0x0fff), p, 2);
        s += sf(" handle %u", le16(p) & 0x0fff);
        if (code == 0x1b && n >= 3) {
          F("Max slots", std::to_string(p[2]), p + 2, 1);
          s += sf(" %u slots", p[2]);
        } else if (code == 0x38 && n >= 4) {
          F("Timeout", sf("%.1f ms", le16(p + 2) * 0.625), p + 2, 2);
        }
      }
      break;
    case 0x3e:
      s = le_meta(o, p, n);
      break;
    case 0xff:
      if (o.manufacturer == kCompanyIntel) {
        s = intel_event(r);
      } else if (n) {
        F("Data", hexbytes(p, n, 64), p, n);
      }
      break;
    default:
      // The rest that start with status + handle.
      switch (code) {
        case 0x06: case 0x09: case 0x30:
          status_handle();
          break;
        case 0x0a:  // Link Key Type Changed
          status_handle();
          if (n >= 4) F("Key flag", p[3] ? "temporary" : "semi-permanent", p + 3, 1);
          break;
        default:
          break;
      }
      break;
  }
  rest(o, p, n);
  CLOSE();
  o.summary = s;
}

// ---------------------------------------------------------------------------------------------
// L2CAP signalling

std::string l2cap_sig(Out& o, const uint8_t* d, size_t len, bool le) {
  std::string s;
  int count = 0;
  while (len >= 4) {
    const uint8_t code = d[0], ident = d[1];
    const size_t clen = le16(d + 2);
    const uint8_t* p = d + 4;
    const size_t n = std::min(clen, len - 4);
    const char* nm = sig_name(code);
    OPEN(nm, sf("ident %u", ident), d, 4 + n);
    F("Code", sf("0x%02x (%s)", code, nm), d, 1);
    F("Identifier", std::to_string(ident), d + 1, 1);
    F("Length", std::to_string(clen), d + 2, 2);
    std::string t = nm;
    switch (code) {
      case 0x01:  // Command Reject
        if (n >= 2) {
          static const char* reasons[] = {"command not understood", "signalling MTU exceeded",
                                          "invalid CID in request"};
          const uint16_t r = le16(p);
          F("Reason", r < 3 ? reasons[r] : "?", p, 2);
          t += std::string(" (") + (r < 3 ? reasons[r] : "?") + ")";
        }
        break;
      case 0x02:  // Connection Request
      case 0x0c:
        if (n >= 4) {
          F("PSM", psm_str(le16(p)), p, 2);
          F("Source CID", sf("0x%04x", le16(p + 2)), p + 2, 2);
          t += " psm " + psm_str(le16(p)) + sf(" scid 0x%04x", le16(p + 2));
        }
        break;
      case 0x03:  // Connection Response
      case 0x0d:
        if (n >= 8) {
          F("Destination CID", sf("0x%04x", le16(p)), p, 2);
          F("Source CID", sf("0x%04x", le16(p + 2)), p + 2, 2);
          F("Result", sf("0x%04x (%s)", le16(p + 4), conn_result_name(le16(p + 4))), p + 4, 2);
          F("Status", sf("0x%04x", le16(p + 6)), p + 6, 2);
          t += sf(" dcid 0x%04x scid 0x%04x %s", le16(p), le16(p + 2), conn_result_name(le16(p + 4)));
        }
        break;
      case 0x04:  // Configuration Request
      case 0x05: {  // Configuration Response
        const bool rsp = code == 0x05;
        if (n < 4 + (rsp ? 2u : 0u)) break;
        F(rsp ? "Source CID" : "Destination CID", sf("0x%04x", le16(p)), p, 2);
        F("Flags", sf("0x%04x", le16(p + 2)), p + 2, 2);
        t += sf(" %s 0x%04x", rsp ? "scid" : "dcid", le16(p));
        size_t at = 4;
        if (rsp) {
          static const char* res[] = {"success", "unacceptable parameters", "rejected",
                                      "unknown options", "pending", "flow spec rejected"};
          const uint16_t r = le16(p + 4);
          F("Result", sf("0x%04x (%s)", r, r < 6 ? res[r] : "?"), p + 4, 2);
          t += std::string(" ") + (r < 6 ? res[r] : "?");
          at = 6;
        }
        while (at + 2 <= n) {
          const uint8_t ot = p[at] & 0x7f;
          const size_t ol = p[at + 1];
          if (at + 2 + ol > n) break;
          const uint8_t* v = p + at + 2;
          switch (ot) {
            case 0x01:
              if (ol >= 2) {
                F("MTU", std::to_string(le16(v)), p + at, 2 + ol);
                t += sf(" mtu %u", le16(v));
              }
              break;
            case 0x02:
              if (ol >= 2) F("Flush timeout", std::to_string(le16(v)), p + at, 2 + ol);
              break;
            case 0x04:
              if (ol >= 1) {
                static const char* modes[] = {"basic", "retransmission", "flow control",
                                              "enhanced retransmission", "streaming"};
                F("Retransmission and flow control", v[0] < 5 ? modes[v[0]] : "?", p + at, 2 + ol);
                if (v[0]) t += std::string(" ") + (v[0] < 5 ? modes[v[0]] : "?");
              }
              break;
            case 0x05:
              if (ol >= 1) F("FCS", v[0] ? "16-bit" : "none", p + at, 2 + ol);
              break;
            default:
              F(sf("Option 0x%02x", ot), hexbytes(v, ol), p + at, 2 + ol);
              break;
          }
          at += 2 + ol;
        }
        break;
      }
      case 0x06:  // Disconnection Request
      case 0x07:
        if (n >= 4) {
          F("Destination CID", sf("0x%04x", le16(p)), p, 2);
          F("Source CID", sf("0x%04x", le16(p + 2)), p + 2, 2);
          t += sf(" dcid 0x%04x scid 0x%04x", le16(p), le16(p + 2));
        }
        break;
      case 0x0a:  // Information Request
      case 0x0b:
        if (n >= 2) {
          static const char* types[] = {"?", "connectionless MTU", "extended features",
                                        "fixed channels"};
          const uint16_t it = le16(p);
          F("Type", it < 4 ? types[it] : "?", p, 2);
          t += std::string(" ") + (it < 4 ? types[it] : "?");
          if (code == 0x0b && n >= 4) {
            F("Result", le16(p + 2) ? "not supported" : "success", p + 2, 2);
            // The answer per type (Core Vol 3 Part A 4.12): an MTU or a bit mask.
            static const char* const feat[] = {"flow control", "retransmission", "bi-directional QoS",
                                               "enhanced retransmission", "streaming", "FCS option",
                                               "extended flow specification", "fixed channels", "extended window size",
                                               "unicast connectionless data", "enhanced credit based flow control"};
            static const char* const fixed[] = {nullptr, "L2CAP signalling", "connectionless", "AMP manager",
                                                nullptr, nullptr, nullptr, "BR/EDR security manager"};
            if (!le16(p + 2) && it == 1 && n >= 6) {
              F("Connectionless MTU", std::to_string(le16(p + 4)), p + 4, 2);
            } else if (!le16(p + 2) && it == 2 && n >= 8) {
              F("Features", sf("0x%08x (%s)", le32(p + 4), bit_names(le32(p + 4), feat, 11).c_str()), p + 4, 4);
            } else if (!le16(p + 2) && it == 3 && n >= 12) {
              uint64_t v = 0;
              for (int i = 7; i >= 0; --i) v = v << 8 | p[4 + i];
              F("Fixed channels", hexbytes(p + 4, 8) + " (" + bit_names(v, fixed, 8) + ")", p + 4, 8);
            } else if (n > 4) {
              F("Data", hexbytes(p + 4, n - 4), p + 4, n - 4);
            }
          }
        }
        break;
      case 0x12:  // Connection Parameter Update Request
        if (n >= 8) {
          F("Interval", sf("%.2f-%.2f ms", le16(p) * 1.25, le16(p + 2) * 1.25), p, 4);
          F("Latency", std::to_string(le16(p + 4)), p + 4, 2);
          F("Timeout", sf("%u ms", le16(p + 6) * 10), p + 6, 2);
          t += sf(" interval %.2f-%.2f ms", le16(p) * 1.25, le16(p + 2) * 1.25);
        }
        break;
      case 0x13:
        if (n >= 2) {
          F("Result", le16(p) ? "rejected" : "accepted", p, 2);
          t += le16(p) ? " rejected" : " accepted";
        }
        break;
      case 0x14:  // LE Credit Based Connection Request
        if (n >= 10) {
          F("SPSM", psm_str(le16(p)), p, 2);
          F("Source CID", sf("0x%04x", le16(p + 2)), p + 2, 2);
          F("MTU", std::to_string(le16(p + 4)), p + 4, 2);
          F("MPS", std::to_string(le16(p + 6)), p + 6, 2);
          F("Credits", std::to_string(le16(p + 8)), p + 8, 2);
          t += " psm " + psm_str(le16(p)) + sf(" scid 0x%04x", le16(p + 2));
        }
        break;
      case 0x15:
        if (n >= 10) {
          F("Destination CID", sf("0x%04x", le16(p)), p, 2);
          F("MTU", std::to_string(le16(p + 2)), p + 2, 2);
          F("MPS", std::to_string(le16(p + 4)), p + 4, 2);
          F("Credits", std::to_string(le16(p + 6)), p + 6, 2);
          F("Result", sf("0x%04x (%s)", le16(p + 8), conn_result_name(le16(p + 8))), p + 8, 2);
          t += sf(" dcid 0x%04x %s", le16(p), conn_result_name(le16(p + 8)));
        }
        break;
      case 0x16:  // Flow Control Credit
        if (n >= 4) {
          F("CID", sf("0x%04x", le16(p)), p, 2);
          F("Credits", std::to_string(le16(p + 2)), p + 2, 2);
          t += sf(" cid 0x%04x +%u", le16(p), le16(p + 2));
        }
        break;
      case 0x17:  // Credit Based Connection Request
        if (n >= 8) {
          F("SPSM", psm_str(le16(p)), p, 2);
          F("MTU", std::to_string(le16(p + 2)), p + 2, 2);
          F("MPS", std::to_string(le16(p + 4)), p + 4, 2);
          F("Credits", std::to_string(le16(p + 6)), p + 6, 2);
          std::string cids;
          for (size_t i = 8; i + 2 <= n; i += 2) cids += sf("%s0x%04x", i > 8 ? " " : "", le16(p + i));
          F("Source CIDs", cids, p + 8, n - 8);
          t += " psm " + psm_str(le16(p)) + " scids " + cids;
        }
        break;
      case 0x18:
        if (n >= 8) {
          F("MTU", std::to_string(le16(p)), p, 2);
          F("MPS", std::to_string(le16(p + 2)), p + 2, 2);
          F("Credits", std::to_string(le16(p + 4)), p + 4, 2);
          F("Result", sf("0x%04x (%s)", le16(p + 6), conn_result_name(le16(p + 6))), p + 6, 2);
          std::string cids;
          for (size_t i = 8; i + 2 <= n; i += 2) cids += sf("%s0x%04x", i > 8 ? " " : "", le16(p + i));
          F("Destination CIDs", cids, p + 8, n - 8);
          t += " dcids " + cids + " " + conn_result_name(le16(p + 6));
        }
        break;
      default:
        if (n) F("Data", hexbytes(p, n), p, n);
        break;
    }
    CLOSE();
    if (count < 3) s += (s.empty() ? "" : "; ") + t;
    ++count;
    if (clen > len - 4) break;
    d += 4 + clen;
    len -= 4 + clen;
  }
  if (count > 3) s += sf("; +%d more", count - 3);
  (void)le;
  return s;
}

// ---------------------------------------------------------------------------------------------
// ATT

// ---------------------------------------------------------------------------------------------
// GATT characteristic values: what btmon's att.c decodes once discovery named the handle. LE Audio
// first (ASCS, PACS, BASS, VCS, CSIS, MCS), then the GAP/GATT and Battery basics.

const char* ase_state_name(uint8_t s) {
  static const char* const names[] = {"Idle", "Codec Configured", "QoS Configured", "Enabling", "Streaming",
                                      "Disabling", "Releasing"};
  return s < 7 ? names[s] : "?";
}

const char* ase_op_name(uint8_t op) {
  static const char* const names[] = {"?", "Config Codec", "Config QoS", "Enable", "Receiver Start Ready",
                                      "Disable", "Receiver Stop Ready", "Update Metadata", "Release"};
  return op < 9 ? names[op] : "?";
}

// ASE Control Point response codes and reasons (ASCS 5.1, Tables 5.1 and 5.2).
const char* ase_rsp_name(uint8_t c) {
  static const char* const names[] = {"success", "unsupported opcode", "invalid length", "invalid ASE ID",
                                      "invalid ASE state machine transition", "invalid ASE direction",
                                      "unsupported audio capabilities", "unsupported configuration parameter value",
                                      "rejected configuration parameter value", "invalid configuration parameter value",
                                      "unsupported metadata", "rejected metadata", "invalid metadata",
                                      "insufficient resources", "unspecified error"};
  return c < 15 ? names[c] : "?";
}
const char* ase_reason_name(uint8_t r) {
  static const char* const names[] = {"none", "codec ID", "codec specific configuration", "SDU interval", "framing",
                                      "PHY", "max SDU", "RTN", "max transport latency", "presentation delay",
                                      "invalid ASE CIS mapping"};
  return r < 11 ? names[r] : "?";
}

// An ASE characteristic (ASCS 4.1): its ID, state and that state's parameters. Returns the
// words for the node's value: "ASE 1 Streaming, CIG 0 CIS 0".
std::string ase_value(Rd& r) {
  const uint8_t id = r.u8("ASE ID");
  const uint8_t st = static_cast<uint8_t>(r.named("State", 1, [](uint32_t v) { return ase_state_name(static_cast<uint8_t>(v)); }));
  std::string s = sf("ASE %u %s", id, ase_state_name(st));
  switch (st) {
    case 1: {  // Codec Configured
      r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "unframed ISOAL PDUs not supported" : "unframed ISOAL PDUs supported"); });
      r.phys("Preferred PHY");
      r.u8("Preferred retransmission number");
      r.u16("Max transport latency", "%u ms");
      r.u24("Presentation delay min", "%u us");
      r.u24("Presentation delay max", "%u us");
      r.u24("Preferred presentation delay min", "%u us");
      r.u24("Preferred presentation delay max", "%u us");
      const std::string c = codec_ltvs(r, Ltv::kCodecConfig, "Codec configuration length", "Codec configuration");
      if (!c.empty()) s += ", " + c;
      break;
    }
    case 2: {  // QoS Configured
      const uint8_t cig = r.u8("CIG ID");
      const uint8_t cis = r.u8("CIS ID");
      r.u24("SDU interval", "%u us");
      r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "framed" : "unframed"); });
      r.phys("PHY");
      r.u16("Max SDU");
      r.u8("Retransmission number");
      r.u16("Max transport latency", "%u ms");
      r.u24("Presentation delay", "%u us");
      s += sf(", CIG %u CIS %u", cig, cis);
      break;
    }
    case 3:
    case 4:
    case 5: {  // Enabling, Streaming, Disabling
      const uint8_t cig = r.u8("CIG ID");
      const uint8_t cis = r.u8("CIS ID");
      std::string meta;
      metadata(r, "Metadata length", &meta);
      s += sf(", CIG %u CIS %u", cig, cis) + (meta.empty() ? "" : ", " + meta);
      break;
    }
    default:
      break;
  }
  return s;
}

// The ASE Control Point (ASCS 5): an operation written by the client, or the server's answer in a
// notification. Returns "Config QoS ASE 1, 2".
std::string ase_cp(Rd& r, bool notify) {
  Out& o = r.o;
  const uint8_t op = static_cast<uint8_t>(r.named("Opcode", 1, [](uint32_t v) { return ase_op_name(static_cast<uint8_t>(v)); }));
  std::string s = ase_op_name(op);
  const uint8_t k = r.with("Number of ASEs", 1, [](uint32_t v) { return v == 0xff ? std::string("0xff (invalid length or opcode)") : std::to_string(v); });
  if (notify) {
    std::string bad;
    for (unsigned i = 0; i < k && k != 0xff && r.has(3); ++i) {
      const uint8_t* e = r.cur();
      OPEN(sf("ASE %u", e[0]), ase_rsp_name(e[1]), e, 3);
      r.u8("ASE ID");
      r.named("Response code", 1, [](uint32_t v) { return ase_rsp_name(static_cast<uint8_t>(v)); });
      r.named("Reason", 1, [](uint32_t v) { return ase_reason_name(static_cast<uint8_t>(v)); });
      CLOSE();
      if (e[1]) bad += sf("%s ASE %u %s", bad.empty() ? "" : ",", e[0], ase_rsp_name(e[1])) + (e[2] ? sf(" (%s)", ase_reason_name(e[2])) : "");
    }
    return s + (bad.empty() ? " success" : ": " + bad);
  }
  std::string ids;
  for (unsigned i = 0; i < k && r.has(1); ++i) {
    const uint8_t* e = r.cur();
    ids += sf("%s%u", ids.empty() ? "" : ", ", e[0]);
    switch (op) {
      case 1: {  // Config Codec
        OPEN(sf("ASE %u", e[0]), "", e, r.left());
        r.u8("ASE ID");
        r.with("Target latency", 1, [](uint32_t v) {
          static const char* const t[] = {"?", "low latency", "balanced", "high reliability"};
          return std::string(v < 4 ? t[v] : "?");
        });
        r.phy("Target PHY");
        codec_ltvs(r, Ltv::kCodecConfig, "Codec configuration length", "Codec configuration");
        CLOSE();
        break;
      }
      case 2:  // Config QoS
        OPEN(sf("ASE %u", e[0]), "", e, std::min<size_t>(16, r.left()));
        r.u8("ASE ID");
        r.u8("CIG ID");
        r.u8("CIS ID");
        r.u24("SDU interval", "%u us");
        r.with("Framing", 1, [](uint32_t v) { return std::string(v ? "framed" : "unframed"); });
        r.phys("PHY");
        r.u16("Max SDU");
        r.u8("Retransmission number");
        r.u16("Max transport latency", "%u ms");
        r.u24("Presentation delay", "%u us");
        CLOSE();
        break;
      case 3:  // Enable
      case 7:  // Update Metadata
        OPEN(sf("ASE %u", e[0]), "", e, r.left());
        r.u8("ASE ID");
        metadata(r);
        CLOSE();
        break;
      default:  // the rest name ASEs only
        r.u8("ASE ID");
        break;
    }
  }
  return s + (ids.empty() ? "" : " ASE " + ids);
}

// Published Audio Capabilities records (PACS 3.1): codec, capabilities and metadata each.
std::string pac_records(Rd& r) {
  Out& o = r.o;
  const uint8_t k = r.u8("Records");
  std::string s = sf("%u record%s", k, k == 1 ? "" : "s");
  for (unsigned i = 0; i < k && r.has(7); ++i) {
    OPEN(sf("Record %u", i), "", r.cur(), r.left());
    const std::string c = codec_ltvs(r, Ltv::kCodecCaps, "Capabilities length", "Capabilities");
    metadata(r);
    CLOSE();
    if (i == 0 && !c.empty()) s += ": " + c;
  }
  return s;
}

const char* pa_sync_state_name(uint8_t s) {
  static const char* const names[] = {"not synchronized", "SyncInfo request", "synchronized", "failed to synchronize",
                                      "no PAST"};
  return s < 5 ? names[s] : "?";
}
const char* big_encryption_name(uint8_t e) {
  static const char* const names[] = {"not encrypted", "broadcast code required", "decrypting", "bad code"};
  return e < 4 ? names[e] : "?";
}

// One subgroup list of BASS (Add/Modify Source, Broadcast Receive State): BIS sync bits, metadata.
void bass_subgroups(Rd& r, bool state) {
  Out& o = r.o;
  const uint8_t k = r.u8("Subgroups");
  for (unsigned i = 0; i < k && r.has(5); ++i) {
    OPEN(sf("Subgroup %u", i), "", r.cur(), r.left());
    r.with(state ? "BIS sync state" : "BIS sync", 4, [state](uint32_t v) {
      if (v == 0xffffffff) return std::string(state ? "failed to sync to the BIG" : "no preference");
      std::string b;
      for (unsigned i = 0; i < 31; ++i)
        if (v >> i & 1) b += sf("%s%u", b.empty() ? "" : ", ", i + 1);
      return sf("0x%08x", v) + (b.empty() ? "" : " (BIS " + b + ")");
    });
    metadata(r);
    CLOSE();
  }
}

// A Broadcast Receive State (BASS 3.2): the source a scan delegator is synchronized to (or none).
std::string bass_state(Rd& r) {
  if (!r.has(1)) return "no source";
  const uint8_t id = r.u8("Source ID");
  r.atype("Source address type");
  const std::string a = r.addr("Source address");
  r.u8("Source advertising SID");
  const uint32_t bid = r.u24("Broadcast ID", "0x%06x");
  const uint8_t pa = static_cast<uint8_t>(r.named("PA sync state", 1, [](uint32_t v) { return pa_sync_state_name(static_cast<uint8_t>(v)); }));
  const uint8_t enc = static_cast<uint8_t>(r.named("BIG encryption", 1, [](uint32_t v) { return big_encryption_name(static_cast<uint8_t>(v)); }));
  if (enc == 3) r.hex("Bad code", 16);
  bass_subgroups(r, true);
  return sf("source %u %s broadcast ID 0x%06x, PA %s, %s", id, a.c_str(), bid, pa_sync_state_name(pa), big_encryption_name(enc));
}

// The Broadcast Audio Scan Control Point (BASS 3.1): what a broadcast assistant asks.
std::string bass_cp(Rd& r) {
  static const char* const ops[] = {"Remote Scan Stopped", "Remote Scan Started", "Add Source", "Modify Source",
                                    "Set Broadcast Code", "Remove Source"};
  const uint8_t op = static_cast<uint8_t>(r.named("Opcode", 1, [](uint32_t v) { return v < 6 ? ops[v] : "?"; }));
  std::string s = op < 6 ? ops[op] : sf("opcode 0x%02x", op);
  auto pa_sync = [](uint32_t v) {
    static const char* const p[] = {"do not synchronize", "synchronize, PAST available", "synchronize, no PAST"};
    return sf("0x%02x (%s)", v, v < 3 ? p[v] : "?");
  };
  auto pa_interval = [](uint32_t v) { return v == 0xffff ? std::string("unknown") : sf("%.2f ms", v * 1.25); };
  switch (op) {
    case 2: {  // Add Source
      r.atype("Advertiser address type");
      const std::string a = r.addr("Advertiser address");
      r.u8("Advertising SID");
      const uint32_t bid = r.u24("Broadcast ID", "0x%06x");
      r.with("PA sync", 1, pa_sync);
      r.with("PA interval", 2, pa_interval);
      bass_subgroups(r, false);
      if (!a.empty()) s += sf(" %s broadcast ID 0x%06x", a.c_str(), bid);
      break;
    }
    case 3:  // Modify Source
      s += sf(" %u", r.u8("Source ID"));
      r.with("PA sync", 1, pa_sync);
      r.with("PA interval", 2, pa_interval);
      bass_subgroups(r, false);
      break;
    case 4:  // Set Broadcast Code
      s += sf(" %u", r.u8("Source ID"));
      r.hex("Broadcast code", 16);
      break;
    case 5:  // Remove Source
      s += sf(" %u", r.u8("Source ID"));
      break;
    default:
      break;
  }
  return s;
}

const char* media_state_name(uint8_t s) {
  static const char* const names[] = {"inactive", "playing", "paused", "seeking"};
  return s < 4 ? names[s] : "?";
}
const char* mcp_op_name(uint8_t op) {
  switch (op) {
    case 0x01: return "Play";
    case 0x02: return "Pause";
    case 0x03: return "Fast Rewind";
    case 0x04: return "Fast Forward";
    case 0x05: return "Stop";
    case 0x10: return "Move Relative";
    case 0x20: return "Previous Segment";
    case 0x21: return "Next Segment";
    case 0x22: return "First Segment";
    case 0x23: return "Last Segment";
    case 0x24: return "Goto Segment";
    case 0x30: return "Previous Track";
    case 0x31: return "Next Track";
    case 0x32: return "First Track";
    case 0x33: return "Last Track";
    case 0x34: return "Goto Track";
    case 0x40: return "Previous Group";
    case 0x41: return "Next Group";
    case 0x42: return "First Group";
    case 0x43: return "Last Group";
    case 0x44: return "Goto Group";
    default: return "?";
  }
}

// A characteristic value by its UUID: the fields in the tree, and a few words for the node.
// Returns false when this UUID has no decoder here (the value stays bytes).
bool gatt_value(Out& o, uint16_t uuid, uint8_t op, const uint8_t* v, size_t n, std::string* words) {
  Rd r(o, v, n);
  const bool notify = op == 0x1b || op == 0x1d || op == 0x23;
  auto text = [&](const char* name) {
    *words = quoted(v, n, 60);
    F(name, quoted(v, n, 400), v, n);
  };
  switch (uuid) {
    case 0x2bc4:  // Sink ASE
    case 0x2bc5:  // Source ASE
      *words = ase_value(r);
      return true;
    case 0x2bc6:  // ASE Control Point
      *words = ase_cp(r, notify);
      return true;
    case 0x2bc9:  // Sink PAC
    case 0x2bcb:  // Source PAC
      *words = pac_records(r);
      return true;
    case 0x2bca:  // Sink Audio Locations
    case 0x2bcc:  // Source Audio Locations
    case 0x2b81:  // Audio Location (VOCS)
      if (n >= 4) *words = locations_short(le32(v));
      r.with("Audio locations", 4, [](uint32_t x) { return locations_str(x); });
      return true;
    case 0x2bcd:  // Available Audio Contexts
    case 0x2bce:  // Supported Audio Contexts
      if (n >= 4) *words = "sink " + bit_names(le16(v), kAudioContexts, 12) + "; source " + bit_names(le16(v + 2), kAudioContexts, 12);
      r.with("Sink contexts", 2, [](uint32_t x) { return contexts_str(static_cast<uint16_t>(x)); });
      r.with("Source contexts", 2, [](uint32_t x) { return contexts_str(static_cast<uint16_t>(x)); });
      return true;
    case 0x2bc7:  // Broadcast Audio Scan Control Point
      *words = bass_cp(r);
      return true;
    case 0x2bc8:  // Broadcast Receive State
      *words = bass_state(r);
      return true;
    case 0x2b7d:  // Volume State
      if (n >= 3) *words = sf("volume %u (%u%%)%s", v[0], (v[0] * 100 + 127) / 255, v[1] ? ", muted" : "");
      r.with("Volume setting", 1, [](uint32_t x) { return sf("%u (%u%%)", x, (x * 100 + 127) / 255); });
      r.yes_no("Mute", "muted", "not muted");
      r.u8("Change counter");
      return true;
    case 0x2b7e: {  // Volume Control Point
      static const char* const ops[] = {"Relative Volume Down", "Relative Volume Up", "Unmute/Relative Volume Down",
                                        "Unmute/Relative Volume Up", "Set Absolute Volume", "Unmute", "Mute"};
      const uint8_t vop = static_cast<uint8_t>(r.named("Opcode", 1, [](uint32_t x) { return x < 7 ? ops[x] : "?"; }));
      r.u8("Change counter");
      *words = vop < 7 ? ops[vop] : "?";
      if (vop == 4 && r.has(1)) *words += sf(" %u", r.cur()[0]);
      if (vop == 4) r.with("Volume setting", 1, [](uint32_t x) { return sf("%u (%u%%)", x, (x * 100 + 127) / 255); });
      return true;
    }
    case 0x2b7f:  // Volume Flags
      r.with("Volume flags", 1, [](uint32_t x) { return sf("0x%02x (volume setting %s)", x, x & 1 ? "persisted" : "reset"); });
      return true;
    case 0x2bc3:  // Mute (MICS)
      if (n >= 1) *words = v[0] == 0 ? "not muted" : v[0] == 1 ? "muted" : "disabled";
      r.with("Mute", 1, [](uint32_t x) { return std::string(x == 0 ? "not muted" : x == 1 ? "muted" : x == 2 ? "disabled" : "?"); });
      return true;
    case 0x2b84:  // Set Identity Resolving Key
      r.with("Type", 1, [](uint32_t x) { return std::string(x == 0 ? "encrypted" : x == 1 ? "plain text" : "?"); });
      r.hex("SIRK", 16);
      return true;
    case 0x2b85:  // Coordinated Set Size
      if (n >= 1) *words = sf("%u members", v[0]);
      r.u8("Set size");
      return true;
    case 0x2b86:  // Set Member Lock
      if (n >= 1) *words = v[0] == 2 ? "locked" : v[0] == 1 ? "unlocked" : "?";
      r.with("Lock", 1, [](uint32_t x) { return std::string(x == 2 ? "locked" : x == 1 ? "unlocked" : "?"); });
      return true;
    case 0x2b87:  // Set Member Rank
      r.u8("Rank");
      return true;
    case 0x2b51:  // TMAP Role
      if (n >= 2) *words = bit_names(le16(v), kTmapRoles, 6);
      r.flags("TMAP role", 2, kTmapRoles, 6);
      return true;
    case 0x2b93:  // Media Player Name
    case 0x2b97:  // Track Title
    case 0x2bb3:  // Bearer Provider Name
    case 0x2bb4:  // Bearer UCI
    case 0x2bc2:  // Call Friendly Name
    case 0x2a00:  // Device Name
    case 0x2a24: case 0x2a25: case 0x2a26: case 0x2a27: case 0x2a28: case 0x2a29:  // Device Information strings
      text("Text");
      return true;
    case 0x2b98:  // Track Duration
    case 0x2b99:  // Track Position
      if (n >= 4) {
        const int32_t t = static_cast<int32_t>(le32(v));
        *words = t == -1 ? std::string("unknown") : sf("%d:%02d.%02d", t / 6000, t / 100 % 60, t % 100);
      }
      r.with(uuid == 0x2b98 ? "Duration" : "Position", 4, [](uint32_t x) {
        const int32_t t = static_cast<int32_t>(x);
        return t == -1 ? std::string("unknown") : sf("%d (x 10 ms: %d:%02d.%02d)", t, t / 6000, t / 100 % 60, t % 100);
      });
      return true;
    case 0x2ba3:  // Media State
      if (n >= 1) *words = media_state_name(v[0]);
      r.named("Media state", 1, [](uint32_t x) { return media_state_name(static_cast<uint8_t>(x)); });
      return true;
    case 0x2ba4: {  // Media Control Point
      const uint8_t mop = static_cast<uint8_t>(r.named("Opcode", 1, [](uint32_t x) { return mcp_op_name(static_cast<uint8_t>(x)); }));
      *words = mcp_op_name(mop);
      if (notify) {
        static const char* const res[] = {"?", "success", "opcode not supported", "media player inactive",
                                          "command cannot be completed"};
        if (r.has(1)) *words += std::string(" ") + (r.cur()[0] < 5 ? res[r.cur()[0]] : "?");
        r.named("Result", 1, [](uint32_t x) { return x < 5 ? res[x] : "?"; });
      } else if (r.has(4)) {
        r.with("Parameter", 4, [](uint32_t x) { return std::to_string(static_cast<int32_t>(x)); });
      }
      return true;
    }
    case 0x2bba:  // Content Control ID
      r.u8("CCID");
      return true;
    case 0x2902: {  // Client Characteristic Configuration
      static const char* const bits[] = {"notification", "indication"};
      if (n >= 2) *words = bit_names(le16(v), bits, 2);
      r.flags("Configuration", 2, bits, 2);
      return true;
    }
    case 0x2a01:  // Appearance
      if (n >= 2) *words = appearance_name(le16(v));
      r.with("Appearance", 2, [](uint32_t x) { return sf("0x%04x (%s)", x, appearance_name(static_cast<uint16_t>(x))); });
      return true;
    case 0x2a04:  // Peripheral Preferred Connection Parameters
      r.time("Min interval", 2, 1.25);
      r.time("Max interval", 2, 1.25);
      r.u16("Latency");
      r.time("Supervision timeout", 2, 10);
      return true;
    case 0x2a05:  // Service Changed
      if (n >= 4) *words = sf("0x%04x-0x%04x", le16(v), le16(v + 2));
      r.u16("Start handle", "0x%04x");
      r.u16("End handle", "0x%04x");
      return true;
    case 0x2a19:  // Battery Level
      if (n >= 1) *words = sf("%u%%", v[0]);
      r.u8("Battery level", "%u%%");
      return true;
    case 0x2a50:  // PnP ID
      r.with("Vendor ID source", 1, [](uint32_t x) { return std::string(x == 1 ? "Bluetooth SIG" : x == 2 ? "USB" : "?"); });
      r.with("Vendor", 2, [](uint32_t x) { const char* c = company_name(static_cast<uint16_t>(x)); return sf("0x%04x", x) + (c ? std::string(" (") + c + ")" : ""); });
      r.u16("Product", "0x%04x");
      r.u16("Version", "0x%04x");
      return true;
    case 0x2aa6:  // Central Address Resolution
    case 0x2ac9:  // Resolvable Private Address Only
      r.yes_no(uuid == 0x2aa6 ? "Address resolution" : "RPA only", "supported", "not supported");
      return true;
    case 0x2b29: {  // Client Supported Features
      static const char* const bits[] = {"robust caching", "EATT", "multiple handle value notifications"};
      if (n >= 1) *words = bit_names(v[0], bits, 3);
      r.flags("Features", 1, bits, 3);
      return true;
    }
    case 0x2b2a:  // Database Hash
      r.hex("Hash", 16);
      return true;
    case 0x2b3a: {  // Server Supported Features
      static const char* const bits[] = {"EATT"};
      if (n >= 1) *words = bit_names(v[0], bits, 1);
      r.flags("Features", 1, bits, 1);
      return true;
    }
    default:
      return false;
  }
}

// PAC records (PACS 3.1): count, then codec ID, capabilities and metadata each, the capabilities a
// list of LTVs that ends where its length says. True if v is exactly that.
bool looks_like_pac(const uint8_t* v, size_t n) {
  if (n < 8 || v[0] < 1 || v[0] > 8) return false;
  size_t at = 1;
  for (unsigned i = 0; i < v[0]; ++i) {
    if (at + 6 > n) return false;
    const size_t end = at + 6 + v[at + 5];
    for (size_t e = at + 6; e < end; e += 1 + v[e]) {
      if (e >= n || v[e] == 0 || e + 1 + v[e] > end) return false;
    }
    if (end >= n) return false;
    at = end + 1 + v[end];
  }
  return at == n;
}

// ASE states and ASE Control Point operations and responses (ASCS 4.1, 5): 0x2bc4 (an ASE, sink or
// source), 0x2bc6 (the control point) or 0 when the bytes do not add up to one exactly.
uint16_t looks_like_ascs(uint8_t op, const uint8_t* v, size_t n) {
  const bool notify = op == 0x1b || op == 0x1d;
  const bool write = op == 0x12 || op == 0x52;
  if (n < 3 || (!notify && !write) || v[0] < 1 || v[0] > 8) return 0;
  if (notify) {
    // A control point response: opcode, count, { ASE ID, response code, reason } each.
    if (v[1] >= 1 && v[1] != 0xff && n == 2 + 3u * v[1]) {
      bool ok = true;
      for (size_t i = 2; i < n; i += 3) ok = ok && v[i + 1] <= 0x0e && v[i + 2] <= 0x0a;
      if (ok) return 0x2bc6;
    }
    // An ASE: ID, state, the state's parameters.
    switch (v[1]) {
      case 1: return n >= 25 && n == 25u + v[24] ? 0x2bc4 : 0;
      case 2: return n == 17 ? 0x2bc4 : 0;
      case 3:
      case 4:
      case 5: return n >= 5 && n == 5u + v[4] ? 0x2bc4 : 0;
      default: return 0;
    }
  }
  // An operation: opcode, count, then per ASE a layout of the opcode's.
  if (v[1] < 1) return 0;
  size_t at = 2;
  for (unsigned i = 0; i < v[1]; ++i) {
    switch (v[0]) {
      case 1:  // ASE ID, target latency, target PHY, codec ID, configuration
        if (at + 9 > n || v[at + 1] < 1 || v[at + 1] > 3) return 0;
        at += 9 + v[at + 8];
        break;
      case 2: at += 16; break;
      case 3:
      case 7:
        if (at + 2 > n) return 0;
        at += 2 + v[at + 1];
        break;
      default: at += 1; break;
    }
    if (at > n) return 0;
  }
  return at == n ? 0x2bc6 : 0;
}

// LE Audio's control traffic when no discovery is in the capture (a client that cached the
// handles never runs it, so a board's own ASCS and PACS are usually unnamed): ASE states, ASE
// Control Point operations and PAC records have layouts whose lengths add up exactly, which other
// values rarely do. Returns the characteristic they look like, 0 when the bytes do not add up. The
// two-byte Idle and Releasing states are left alone: a CCC or a volume looks the same.
uint16_t guess_le_audio(uint8_t op, const uint8_t* v, size_t n) {
  if (const uint16_t u = looks_like_ascs(op, v, n)) return u;
  return (op == 0x0b || op == 0x1b || op == 0x1d) && looks_like_pac(v, n) ? 0x2bc9 : 0;
}

std::string char_props_str(uint8_t p) {
  static const char* const names[] = {"broadcast", "read", "write without response", "write", "notify",
                                      "indicate", "authenticated signed writes", "extended properties"};
  return sf("0x%02x (%s)", p, bit_names(p, names, 8).c_str());
}

std::string att(Out& o, const uint8_t* d, size_t n, const GattDb* g, bool tx) {
  if (n < 1) return "ATT (empty)";
  const uint8_t op = d[0];
  const char* nm = att_op_name(op);
  std::string s = nm ? nm : sf("ATT opcode 0x%02x", op);
  OPEN("ATT", s, d, n);
  F("Opcode", sf("0x%02x (%s)", op, nm ? nm : "unknown"), d, 1);
  const uint8_t* p = d + 1;
  const size_t m = n - 1;
  // The attribute server this PDU is about: the sender of a response or notification (the odd
  // opcodes), the receiver of a request.
  const bool local = ((op & 1) != 0) == tx;
  const GattDb::Side* side = g ? &g->side[local ? 1 : 0] : nullptr;
  // "0x0098 (ASE Control Point)" when discovery named the handle.
  auto handle_str = [&](uint16_t h) {
    const uint16_t u = g ? g->uuid(local, h) : 0;
    const char* un = u ? uuid16_name(u) : nullptr;
    if (u == 0x2902) {
      const uint16_t c = g->uuid(local, g->owner(local, h));
      const char* cn = c ? uuid16_name(c) : nullptr;
      if (cn) return sf("0x%04x (Client Characteristic Configuration of %s)", h, cn);
    }
    return un ? sf("0x%04x (%s)", h, un) : u ? sf("0x%04x (0x%04x)", h, u) : sf("0x%04x", h);
  };
  // A value: decoded by its characteristic when discovery named the handle (h; 0 when unknown),
  // as bytes (and text) otherwise. Returns the summary's words, which never depend on discovery.
  auto value = [&](uint16_t h, const uint8_t* v, size_t vn) {
    uint16_t u = o.detail && g && h ? g->uuid(local, h) : 0;
    const bool guessed = o.detail && !u && (u = guess_le_audio(op, v, vn)) != 0;
    std::string words;
    // The node's value is the decoder's summary words, known only once it ran: a first pass
    // without the tree gets them (detail mode only, one packet).
    bool decoded = false;
    if (u) {
      Out probe(v, false);
      decoded = gatt_value(probe, u, op, v, vn, &words);
    }
    if (decoded) {
      const char* un = guessed ? (u == 0x2bc4 ? "ASE" : u == 0x2bc6 ? "ASE Control Point" : "PAC") : uuid16_name(u);
      OPEN(guessed ? std::string(un) + " (by its layout)" : un ? un : sf("0x%04x", u), words, v, vn);
      o.mark(v);
      gatt_value(o, u, op, v, vn, &words);
      rest(o, v, vn, "Value");
      CLOSE();
    } else {
      F("Value", hexbytes(v, vn, 64) + (mostly_text(v, vn) ? "  \"" + printable(v, vn) + "\"" : ""), v, vn);
    }
    return " " + hexbytes(v, vn, 16) + (vn && mostly_text(v, vn) ? " \"" + printable(v, vn, 24) + "\"" : "");
  };
  switch (op) {
    case 0x01:
      if (m >= 4) {
        const char* req = att_op_name(p[0]);
        F("Request", sf("0x%02x (%s)", p[0], req ? req : "?"), p, 1);
        F("Handle", handle_str(le16(p + 1)), p + 1, 2);
        F("Error", sf("0x%02x (%s)", p[3], att_error_name(p[3])), p + 3, 1);
        s += sf(": %s handle 0x%04x %s", req ? req : "?", le16(p + 1), att_error_name(p[3]));
      }
      break;
    case 0x02:
    case 0x03:
      if (m >= 2) {
        F(op == 0x02 ? "Client RX MTU" : "Server RX MTU", std::to_string(le16(p)), p, 2);
        s += sf(" mtu %u", le16(p));
      }
      break;
    case 0x04:
    case 0x06:
    case 0x08:
    case 0x10:
      if (m >= 4) {
        F("Start handle", sf("0x%04x", le16(p)), p, 2);
        F("End handle", sf("0x%04x", le16(p + 2)), p + 2, 2);
        s += sf(" 0x%04x-0x%04x", le16(p), le16(p + 2));
        if (op == 0x06 && m >= 6) {
          F("Type", uuid_str(p + 4, 2), p + 4, 2);
          s += " " + uuid_str(p + 4, 2);
          if (m > 6) F("Value", hexbytes(p + 6, m - 6, 64), p + 6, m - 6);
        } else if ((op == 0x08 || op == 0x10) && (m == 6 || m == 20)) {
          F("Type", uuid_str(p + 4, m - 4), p + 4, m - 4);
          s += " " + uuid_str(p + 4, m - 4);
        }
      }
      break;
    case 0x05:  // Find Information Response
      if (m >= 1) {
        const size_t ul = p[0] == 1 ? 2 : 16;
        F("Format", p[0] == 1 ? "16-bit UUIDs" : "128-bit UUIDs", p, 1);
        size_t cnt = 0;
        for (size_t i = 1; i + 2 + ul <= m; i += 2 + ul, ++cnt) {
          F(sf("Handle 0x%04x", le16(p + i)), uuid_str(p + i + 2, ul), p + i, 2 + ul);
          if (cnt == 0) s += sf(" 0x%04x: %s", le16(p + i), uuid_str(p + i + 2, ul).c_str());
        }
        if (cnt > 1) s += sf(" (+%zu more)", cnt - 1);
      }
      break;
    case 0x07: {  // Find By Type Value Response: handle ranges
      size_t cnt = 0;
      for (size_t i = 0; i + 4 <= m; i += 4, ++cnt) {
        F(sf("Handles %zu", cnt), sf("0x%04x-0x%04x", le16(p + i), le16(p + i + 2)), p + i, 4);
        if (cnt == 0) s += sf(" 0x%04x-0x%04x", le16(p + i), le16(p + i + 2));
      }
      if (cnt > 1) s += sf(" (+%zu more)", cnt - 1);
      break;
    }
    case 0x09:  // Read By Type Response
    case 0x11: {  // Read By Group Type Response
      if (m < 1 || p[0] < 2) break;
      const size_t el = p[0];
      F("Length", std::to_string(el), p, 1);
      // What the request asked for: a Characteristic or Include declaration, else a value. Without
      // the request (a capture that starts later), the lengths of the declarations tell.
      const uint16_t type = side ? side->type_req : 0;
      const bool decl = type == 0x2803 || (!type && (el == 7 || el == 21));
      const bool incl = type == 0x2802 || (!type && (el == 8 || el == 6) && op == 0x09);
      size_t cnt = 0;
      for (size_t i = 1; i + el <= m; i += el, ++cnt) {
        const uint8_t* e = p + i;
        if (op == 0x11 && el >= 6) {
          F(sf("0x%04x-0x%04x", le16(e), le16(e + 2)), uuid_str(e + 4, el - 4), e, el);
          if (cnt == 0) s += sf(" 0x%04x-0x%04x %s", le16(e), le16(e + 2), uuid_str(e + 4, el - 4).c_str());
          continue;
        }
        if (o.detail && decl && (el == 7 || el == 21)) {
          OPEN(sf("Handle 0x%04x", le16(e)), "Characteristic " + uuid_str(e + 5, el - 5), e, el);
          F("Properties", char_props_str(e[2]), e + 2, 1);
          F("Value handle", sf("0x%04x", le16(e + 3)), e + 3, 2);
          F("UUID", uuid_str(e + 5, el - 5), e + 5, el - 5);
          CLOSE();
        } else if (o.detail && incl && (el == 8 || el == 6)) {
          OPEN(sf("Handle 0x%04x", le16(e)), el == 8 ? "Include " + uuid_str(e + 6, 2) : std::string("Include"), e, el);
          F("Included service", sf("0x%04x-0x%04x", le16(e + 2), le16(e + 4)), e + 2, 4);
          if (el == 8) F("UUID", uuid_str(e + 6, 2), e + 6, 2);
          CLOSE();
        } else if (o.detail && type && !decl && !incl) {
          // A value read by its type (Read By Type for a characteristic UUID).
          std::string words;
          Out probe(e + 2, false);
          if (gatt_value(probe, type, op, e + 2, el - 2, &words)) {
            OPEN(sf("Handle 0x%04x", le16(e)), words, e, el);
            o.mark(e + 2);
            gatt_value(o, type, op, e + 2, el - 2, &words);
            rest(o, e + 2, el - 2, "Value");
            CLOSE();
          } else {
            F(sf("Handle 0x%04x", le16(e)), hexbytes(e + 2, el - 2, 64), e, el);
          }
        } else {
          F(sf("Handle 0x%04x", le16(e)), hexbytes(e + 2, el - 2, 64), e, el);
        }
        if (cnt == 0) s += sf(" 0x%04x:", le16(e)) + " " + hexbytes(e + 2, el - 2, 12);
      }
      if (cnt > 1) s += sf(" (+%zu more)", cnt - 1);
      break;
    }
    case 0x0a:  // Read Request
      if (m >= 2) {
        F("Handle", handle_str(le16(p)), p, 2);
        s += sf(" handle 0x%04x", le16(p));
      }
      break;
    case 0x0c:  // Read Blob Request
      if (m >= 4) {
        F("Handle", handle_str(le16(p)), p, 2);
        F("Offset", std::to_string(le16(p + 2)), p + 2, 2);
        s += sf(" handle 0x%04x offset %u", le16(p), le16(p + 2));
      }
      break;
    case 0x0b: {  // Read Response: the handle is the request's
      const uint16_t h = side ? side->read_handle : 0;
      if (h) F("Handle", handle_str(h) + " (from the request)", nullptr, 0);
      s += value(h, p, m);
      break;
    }
    case 0x0d:  // Read Blob Response: a part of a value, bytes only
    case 0x0f:
    case 0x21:
      s += value(0, p, m);
      break;
    case 0x0e:
    case 0x20: {
      std::string hs;
      for (size_t i = 0; i + 2 <= m; i += 2) hs += sf(" 0x%04x", le16(p + i));
      F("Handles", hs, p, m);
      s += hs;
      break;
    }
    case 0x12:  // Write Request
    case 0x52:  // Write Command
    case 0x1b:  // Notification
    case 0x1d:  // Indication
    case 0xd2:
      if (m >= 2) {
        F("Handle", handle_str(le16(p)), p, 2);
        s += sf(" handle 0x%04x", le16(p));
        const size_t vn = op == 0xd2 && m >= 14 ? m - 14 : m - 2;
        s += value(le16(p), p + 2, vn);
        if (op == 0xd2 && m >= 14) F("Signature", hexbytes(p + 2 + vn, 12), p + 2 + vn, 12);
      }
      break;
    case 0x16:  // Prepare Write
    case 0x17:
      if (m >= 4) {
        F("Handle", handle_str(le16(p)), p, 2);
        F("Offset", std::to_string(le16(p + 2)), p + 2, 2);
        s += sf(" handle 0x%04x offset %u", le16(p), le16(p + 2));
        s += value(0, p + 4, m - 4);
      }
      break;
    case 0x18:
      if (m >= 1) {
        F("Flags", p[0] ? "write" : "cancel", p, 1);
        s += p[0] ? " write" : " cancel";
      }
      break;
    case 0x23:  // Multiple Handle Value Notification: { handle, length, value }...
      for (size_t i = 0; i + 4 <= m;) {
        const size_t vl = le16(p + i + 2);
        if (i + 4 + vl > m) break;
        OPEN(sf("Handle 0x%04x", le16(p + i)), handle_str(le16(p + i)), p + i, 4 + vl);
        F("Length", std::to_string(vl), p + i + 2, 2);
        value(le16(p + i), p + i + 4, vl);
        CLOSE();
        if (i == 0) s += sf(" handle 0x%04x", le16(p + i));
        i += 4 + vl;
      }
      break;
    default:
      if (m) F("Data", hexbytes(p, m, 64), p, m);
      break;
  }
  CLOSE();
  return s;
}

// ---------------------------------------------------------------------------------------------
// SMP

const char* smp_name(uint8_t c) {
  switch (c) {
    case 0x01: return "Pairing Request";
    case 0x02: return "Pairing Response";
    case 0x03: return "Pairing Confirm";
    case 0x04: return "Pairing Random";
    case 0x05: return "Pairing Failed";
    case 0x06: return "Encryption Information";
    case 0x07: return "Central Identification";
    case 0x08: return "Identity Information";
    case 0x09: return "Identity Address Information";
    case 0x0a: return "Signing Information";
    case 0x0b: return "Security Request";
    case 0x0c: return "Pairing Public Key";
    case 0x0d: return "Pairing DHKey Check";
    case 0x0e: return "Pairing Keypress Notification";
    default: return nullptr;
  }
}

const char* smp_reason(uint8_t r) {
  switch (r) {
    case 0x01: return "Passkey Entry Failed";
    case 0x02: return "OOB Not Available";
    case 0x03: return "Authentication Requirements";
    case 0x04: return "Confirm Value Failed";
    case 0x05: return "Pairing Not Supported";
    case 0x06: return "Encryption Key Size";
    case 0x07: return "Command Not Supported";
    case 0x08: return "Unspecified Reason";
    case 0x09: return "Repeated Attempts";
    case 0x0a: return "Invalid Parameters";
    case 0x0b: return "DHKey Check Failed";
    case 0x0c: return "Numeric Comparison Failed";
    case 0x0d: return "BR/EDR Pairing In Progress";
    case 0x0e: return "Cross-transport Key Derivation Not Allowed";
    case 0x0f: return "Key Rejected";
    default: return "?";
  }
}

std::string auth_req_str(uint8_t a) {
  std::string s = sf("0x%02x", a);
  if (a & 0x01) s += " bonding";
  if (a & 0x04) s += " MITM";
  if (a & 0x08) s += " SC";
  if (a & 0x10) s += " keypress";
  if (a & 0x20) s += " CT2";
  return s;
}

std::string key_dist_str(uint8_t k) {
  std::string s = sf("0x%02x", k);
  if (k & 0x01) s += " EncKey";
  if (k & 0x02) s += " IdKey";
  if (k & 0x04) s += " SignKey";
  if (k & 0x08) s += " LinkKey";
  return s;
}

std::string smp(Out& o, const uint8_t* d, size_t n) {
  if (n < 1) return "SMP (empty)";
  const char* nm = smp_name(d[0]);
  std::string s = nm ? nm : sf("SMP code 0x%02x", d[0]);
  OPEN("SMP", s, d, n);
  F("Code", sf("0x%02x (%s)", d[0], nm ? nm : "unknown"), d, 1);
  const uint8_t* p = d + 1;
  const size_t m = n - 1;
  switch (d[0]) {
    case 0x01:
    case 0x02:
      if (m >= 6) {
        F("IO capability", io_cap_name(p[0]), p, 1);
        F("OOB data", p[1] ? "present" : "not present", p + 1, 1);
        F("Authentication", auth_req_str(p[2]), p + 2, 1);
        F("Max key size", std::to_string(p[3]), p + 3, 1);
        F("Initiator keys", key_dist_str(p[4]), p + 4, 1);
        F("Responder keys", key_dist_str(p[5]), p + 5, 1);
        s += std::string(" ") + io_cap_name(p[0]) + " " + auth_req_str(p[2]) + sf(", key size %u", p[3]);
        if (p[1]) s += ", OOB";
      }
      break;
    case 0x05:
      if (m >= 1) {
        F("Reason", sf("0x%02x (%s)", p[0], smp_reason(p[0])), p, 1);
        s += std::string(": ") + smp_reason(p[0]);
      }
      break;
    case 0x09:
      if (m >= 7) {
        F("Address type", addr_type_name(p[0]), p, 1);
        F("Address", bdaddr_str(p + 1), p + 1, 6);
        s += " " + bdaddr_str(p + 1);
      }
      break;
    case 0x0b:
      if (m >= 1) {
        F("Authentication", auth_req_str(p[0]), p, 1);
        s += " " + auth_req_str(p[0]);
      }
      break;
    default:
      if (m) F("Data", hexbytes(p, m, 64), p, m);
      break;
  }
  CLOSE();
  return s;
}

// ---------------------------------------------------------------------------------------------
// SDP

// The UUIDs in a data element (the search pattern or an attribute list): enough to tell which
// service a search was for.
void de_uuids(const uint8_t* p, size_t n, std::string* out, int depth = 0) {
  while (n >= 1 && depth < 4) {
    const uint8_t type = p[0] >> 3, sz = p[0] & 7;
    size_t hdr = 1, dl = 0;
    if (sz < 5) {
      dl = type == 0 ? 0 : (1u << sz);
    } else if (sz == 5) {
      if (n < 2) return;
      dl = p[1];
      hdr = 2;
    } else if (sz == 6) {
      if (n < 3) return;
      dl = be16(p + 1);
      hdr = 3;
    } else {
      if (n < 5) return;
      dl = be32(p + 1);
      hdr = 5;
    }
    if (hdr + dl > n) return;
    const uint8_t* v = p + hdr;
    if (type == 3) {  // UUID
      if (dl == 2) {
        const uint16_t u = be16(v);
        const char* nm = uuid16_name(u);
        *out += sf(" 0x%04x", u) + (nm ? std::string(" (") + nm + ")" : "");
      } else if (dl == 4) {
        *out += sf(" 0x%08x", be32(v));
      } else if (dl == 16) {
        *out += " " + hexbytes(v, 16);
      }
    } else if (type == 6 || type == 7) {
      de_uuids(v, dl, out, depth + 1);
    }
    p += hdr + dl;
    n -= hdr + dl;
  }
}

// One data element's header: type, the offset of its value and the value's length. false if it
// does not fit in n.
bool de_header(const uint8_t* p, size_t n, uint8_t* type, size_t* hdr, size_t* len) {
  if (n < 1) return false;
  *type = p[0] >> 3;
  const uint8_t sz = p[0] & 7;
  if (sz < 5) {
    *hdr = 1;
    *len = *type == 0 ? 0 : (1u << sz);
  } else if (sz == 5) {
    if (n < 2) return false;
    *hdr = 2;
    *len = p[1];
  } else if (sz == 6) {
    if (n < 3) return false;
    *hdr = 3;
    *len = be16(p + 1);
  } else {
    if (n < 5) return false;
    *hdr = 5;
    *len = be32(p + 1);
  }
  return *hdr + *len <= n;
}

// A data element as text: integers in hex, UUIDs by name, strings quoted, the outer sequence's
// elements separated by commas and inner sequences in parentheses.
std::string de_str(const uint8_t* p, size_t n, int depth = 0) {
  uint8_t type;
  size_t hdr, len;
  if (!de_header(p, n, &type, &hdr, &len)) return "(truncated)";
  const uint8_t* v = p + hdr;
  switch (type) {
    case 0: return "nil";
    case 1:
    case 2:
      if (len == 1) return sf("0x%02x", v[0]);
      if (len == 2) return sf("0x%04x", be16(v));
      if (len == 4) return sf("0x%08x", be32(v));
      return "0x" + hexbytes(v, len);
    case 3:
      if (len == 2) {
        const char* nm = uuid16_name(be16(v));
        return nm ? std::string(nm) : sf("0x%04x", be16(v));
      }
      if (len == 4) return sf("0x%08x", be32(v));
      if (len == 16) {
        return sf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]) +
               hexbytes(v + 10, 6);
      }
      return hexbytes(v, len);
    case 4:
    case 8: return quoted(v, len, 100);
    case 5: return len && v[0] ? "true" : "false";
    case 6:
    case 7: {
      if (depth > 4) return "(…)";
      std::string s;
      size_t at = 0;
      while (at < len) {
        uint8_t t2;
        size_t h2, l2;
        if (!de_header(v + at, len - at, &t2, &h2, &l2)) break;
        s += (s.empty() ? "" : depth ? " " : ", ") + de_str(v + at, h2 + l2, depth + 1);
        at += h2 + l2;
      }
      return depth ? "(" + s + ")" : s;
    }
    default: return hexbytes(v, len);
  }
}

const char* sdp_attr_name(uint16_t id) {
  switch (id) {
    case 0x0000: return "Service Record Handle";
    case 0x0001: return "Service Class ID List";
    case 0x0002: return "Service Record State";
    case 0x0003: return "Service ID";
    case 0x0004: return "Protocol Descriptor List";
    case 0x0005: return "Browse Group List";
    case 0x0006: return "Language Base Attribute ID List";
    case 0x0007: return "Service Info Time To Live";
    case 0x0008: return "Service Availability";
    case 0x0009: return "Bluetooth Profile Descriptor List";
    case 0x000a: return "Documentation URL";
    case 0x000b: return "Client Executable URL";
    case 0x000c: return "Icon URL";
    case 0x000d: return "Additional Protocol Descriptor Lists";
    case 0x0100: return "Service Name";
    case 0x0101: return "Service Description";
    case 0x0102: return "Provider Name";
    case 0x0200: return "GoepL2capPsm / Version";
    case 0x0311: return "Supported Features";
    case 0x0314: return "Supported Repositories / MAS Instance";
    case 0x0317: return "Supported Features (PBAP/MAP)";
    default: return nullptr;
  }
}

// The words of a protocol descriptor list worth a summary: "PSM 0x0019", "channel 3".
std::string sdp_protocols(const uint8_t* p, size_t n) {
  std::string out;
  uint8_t type;
  size_t hdr, len;
  if (!de_header(p, n, &type, &hdr, &len) || (type != 6 && type != 7)) return out;
  const uint8_t* v = p + hdr;
  size_t at = 0;
  while (at < len) {  // each protocol: (UUID [parameter ...])
    uint8_t t2, t3, t4;
    size_t h2, l2, h3, l3, h4, l4;
    if (!de_header(v + at, len - at, &t2, &h2, &l2)) break;
    const uint8_t* q = v + at + h2;
    if ((t2 == 6 || t2 == 7) && de_header(q, l2, &t3, &h3, &l3) && t3 == 3 && l3 == 2 &&
        h3 + l3 < l2 && de_header(q + h3 + l3, l2 - h3 - l3, &t4, &h4, &l4) && t4 == 1) {
      const uint16_t u = be16(q + h3);
      const uint8_t* pv = q + h3 + l3 + h4;
      const unsigned val = l4 == 1 ? pv[0] : l4 == 2 ? be16(pv) : 0;
      if (u == 0x0100) out += sf(" PSM 0x%04x", val);
      if (u == 0x0003) out += sf(" channel %u", val);
    }
    at += h2 + l2;
  }
  return out;
}

// One service record's attribute list (attribute ID, value, ...): every attribute in the tree,
// and "Audio Source PSM 0x0019 1.3"-style words for the summary.
std::string sdp_record(Out& o, const uint8_t* p, size_t n) {
  std::string cls, extra, name;
  size_t at = 0;
  while (at < n) {
    uint8_t t1, t2;
    size_t h1, l1, h2, l2;
    if (!de_header(p + at, n - at, &t1, &h1, &l1) || t1 != 1 || l1 != 2) break;
    const uint16_t id = be16(p + at + h1);
    const uint8_t* v = p + at + h1 + l1;
    const size_t vn = n - at - h1 - l1;
    if (!de_header(v, vn, &t2, &h2, &l2)) {
      F(sdp_attr_name(id) ? sdp_attr_name(id) : sf("Attribute 0x%04x", id), "(continues in the next response)", p + at, n - at);
      break;
    }
    const std::string val = de_str(v, h2 + l2);
    const char* an = sdp_attr_name(id);
    F(an ? sf("%s (0x%04x)", an, id) : sf("Attribute 0x%04x", id), val, p + at, h1 + l1 + h2 + l2);
    if (id == 0x0001 && cls.empty()) {
      // The first class is the most specific one.
      uint8_t t3;
      size_t h3, l3;
      if ((t2 == 6 || t2 == 7) && de_header(v + h2, l2, &t3, &h3, &l3)) cls = de_str(v + h2, h3 + l3);
    } else if (id == 0x0004 || id == 0x000d) {
      extra += sdp_protocols(id == 0x000d ? v + h2 : v, id == 0x000d ? l2 : h2 + l2);
    } else if (id == 0x0009 && (t2 == 6 || t2 == 7)) {
      // ((profile UUID version) ...): the version of the first profile.
      uint8_t t3, t4, t5;
      size_t h3, l3, h4, l4, h5, l5;
      const uint8_t* q = v + h2;
      if (de_header(q, l2, &t3, &h3, &l3) && (t3 == 6 || t3 == 7) && de_header(q + h3, l3, &t4, &h4, &l4) &&
          h4 + l4 < l3 && de_header(q + h3 + h4 + l4, l3 - h4 - l4, &t5, &h5, &l5) && t5 == 1 && l5 == 2) {
        const uint16_t ver = be16(q + h3 + h4 + l4 + h5);
        const std::string prof = de_str(q + h3, h4 + l4);
        extra += (prof == cls ? std::string(" v") : " " + prof + " ") + sf("%u.%u", ver >> 8, ver & 0xff);
      }
    } else if (id == 0x0100 && (t2 == 4 || t2 == 8)) {
      name = " " + val;
    } else if (id == 0x0200 && t2 == 1 && l2 == 2 && be16(v + h2) >= 0x1001) {
      extra += sf(" GOEP PSM 0x%04x", be16(v + h2));
    }
    at += h1 + l1 + h2 + l2;
  }
  return (cls.empty() ? "?" : cls) + name + extra;
}

// The attribute lists of a Service Attribute Response (one record) or a Service Search
// Attribute Response (a sequence of records). bytes: the AttributeList(s) byte count; when they
// do not end in this PDU (a continuation state follows), only the complete records are decoded.
std::string sdp_attr_lists(Out& o, const uint8_t* p, size_t n, bool search) {
  uint8_t type = 0;
  size_t hdr = 0, len = 0;
  if (n < 2 || ((p[0] >> 3) != 6 && (p[0] >> 3) != 7)) return "";
  if (!de_header(p, n, &type, &hdr, &len)) {
    // Split across responses (a continuation state follows): this is the first part when it starts
    // with the lists' sequence header, and the records complete in it are decoded.
    if (hdr == 0 || hdr >= n || !search || ((p[hdr] >> 3) != 6 && (p[hdr] >> 3) != 7)) {
      F("Attribute lists", "(part of the lists the previous responses began)", p, n);
      return "";
    }
    len = n - hdr;
  }
  if (!search) {
    OPEN("Record", "", p, hdr + len);
    const std::string r = sdp_record(o, p + hdr, len);
    CLOSE();
    return r;
  }
  std::string s;
  size_t at = 0;
  unsigned i = 0;
  while (at < len) {
    uint8_t t2;
    size_t h2, l2;
    if (!de_header(p + hdr + at, len - at, &t2, &h2, &l2)) {
      F(sf("Record %u", i), "(continues in the next response)", p + hdr + at, len - at);
      break;
    }
    OPEN(sf("Record %u", i), "", p + hdr + at, h2 + l2);
    const std::string r = sdp_record(o, p + hdr + at + h2, l2);
    CLOSE();
    s += (s.empty() ? "" : "; ") + r;
    at += h2 + l2;
    ++i;
  }
  return s;
}

std::string sdp(Out& o, const uint8_t* d, size_t n) {
  static const char* names[] = {"?", "Error Response", "Service Search Request",
                                "Service Search Response", "Service Attribute Request",
                                "Service Attribute Response", "Service Search Attribute Request",
                                "Service Search Attribute Response"};
  if (n < 5) return "SDP (truncated)";
  const uint8_t pdu = d[0];
  const char* nm = pdu < 8 ? names[pdu] : "?";
  std::string s = sf("%s (tid %u)", nm, be16(d + 1));
  OPEN("SDP", nm, d, n);
  F("PDU", sf("0x%02x (%s)", pdu, nm), d, 1);
  F("Transaction", std::to_string(be16(d + 1)), d + 1, 2);
  F("Parameter length", std::to_string(be16(d + 3)), d + 3, 2);
  const uint8_t* p = d + 5;
  const size_t m = n - 5;
  // The continuation state ends every PDU but the error: its length, then that many bytes.
  auto continuation = [&](size_t at) {
    if (at < m) {
      F("Continuation state", p[at] ? sf("%u bytes: ", p[at]) + hexbytes(p + at + 1, std::min<size_t>(p[at], m - at - 1)) : "none",
        p + at, m - at);
      if (p[at]) s += pdu & 1 ? " (continues)" : " (continuation)";
    }
  };
  if (pdu == 0x01 && m >= 2) {
    static const char* errs[] = {"?", "Invalid/unsupported SDP version", "Invalid Service Record Handle",
                                 "Invalid request syntax", "Invalid PDU Size", "Invalid Continuation State",
                                 "Insufficient Resources"};
    const uint16_t e = be16(p);
    F("Error", sf("0x%04x (%s)", e, e < 7 ? errs[e] : "?"), p, 2);
    s += sf(" error 0x%04x (%s)", e, e < 7 ? errs[e] : "?");
  } else if (pdu == 0x02 || pdu == 0x04 || pdu == 0x06) {
    size_t at = 0;
    uint8_t type;
    size_t hdr, len;
    if (pdu == 0x04 && m >= 4) {
      F("Record handle", sf("0x%08x", be32(p)), p, 4);
      s += sf(" record 0x%08x", be32(p));
      at = 4;
    } else if (de_header(p, m, &type, &hdr, &len)) {
      std::string u;
      de_uuids(p, hdr + len, &u);
      F("Search pattern", de_str(p, hdr + len), p, hdr + len);
      s += u;
      at = hdr + len;
    }
    if (pdu != 0x04 && at + 2 <= m) {
      F("Maximum record count", std::to_string(be16(p + at)), p + at, 2);
      at += 2;
    } else if (pdu == 0x04 && at + 2 <= m) {
      F("Maximum attribute byte count", std::to_string(be16(p + at)), p + at, 2);
      at += 2;
    }
    if (pdu == 0x06 && at + 2 <= m) {
      F("Maximum attribute byte count", std::to_string(be16(p + at)), p + at, 2);
      at += 2;
    }
    if (pdu != 0x02 && de_header(p + at, m - at, &type, &hdr, &len)) {
      F("Attribute ID list", de_str(p + at, hdr + len), p + at, hdr + len);
      at += hdr + len;
    }
    continuation(at);
  } else if ((pdu == 0x05 || pdu == 0x07) && m >= 2) {
    const size_t bc = std::min<size_t>(be16(p), m - 2);
    F("Byte count", std::to_string(be16(p)), p, 2);
    s += sf(" %u bytes", be16(p));
    const std::string r = sdp_attr_lists(o, p + 2, bc, pdu == 0x07);
    if (!r.empty()) s += ": " + r;
    continuation(2 + bc);
  } else if (pdu == 0x03 && m >= 4) {
    F("Total records", std::to_string(be16(p)), p, 2);
    F("Current records", std::to_string(be16(p + 2)), p + 2, 2);
    s += sf(" %u records", be16(p + 2));
    for (size_t i = 0; i < be16(p + 2) && 4 + 4 * (i + 1) <= m; ++i) {
      F("Record handle", sf("0x%08x", be32(p + 4 + 4 * i)), p + 4 + 4 * i, 4);
    }
    continuation(4 + 4u * be16(p + 2));
  }
  CLOSE();
  return s;
}

// ---------------------------------------------------------------------------------------------
// OBEX (IrOBEX 1.5, GOEP): over RFCOMM, or over an L2CAP channel of its own

const char* obex_opcode_name(uint8_t op) {
  switch (op & 0x7f) {
    case 0x00: return "Connect";
    case 0x01: return "Disconnect";
    case 0x02: return "Put";
    case 0x03: return "Get";
    case 0x05: return "SetPath";
    case 0x06: return "Action";
    case 0x07: return "Session";
    case 0x7f: return "Abort";
    default: return nullptr;
  }
}

const char* obex_response_name(uint8_t rc) {
  switch (rc & 0x7f) {
    case 0x10: return "Continue";
    case 0x20: return "Success";
    case 0x21: return "Created";
    case 0x22: return "Accepted";
    case 0x24: return "No Content";
    case 0x40: return "Bad Request";
    case 0x41: return "Unauthorized";
    case 0x43: return "Forbidden";
    case 0x44: return "Not Found";
    case 0x45: return "Method Not Allowed";
    case 0x46: return "Not Acceptable";
    case 0x48: return "Request Timeout";
    case 0x49: return "Conflict";
    case 0x4b: return "Length Required";
    case 0x4c: return "Precondition Failed";
    case 0x4d: return "Request Entity Too Large";
    case 0x4f: return "Unsupported Media Type";
    case 0x50: return "Internal Server Error";
    case 0x51: return "Not Implemented";
    case 0x53: return "Service Unavailable";
    case 0x60: return "Database Full";
    case 0x61: return "Database Locked";
    default: return nullptr;
  }
}

const char* obex_header_name(uint8_t hi) {
  switch (hi) {
    case 0xc0: return "Count";
    case 0x01: return "Name";
    case 0x42: return "Type";
    case 0xc3: return "Length";
    case 0x44: return "Time";
    case 0xc4: return "Time";
    case 0x05: return "Description";
    case 0x46: return "Target";
    case 0x47: return "HTTP";
    case 0x48: return "Body";
    case 0x49: return "End of Body";
    case 0x4a: return "Who";
    case 0xcb: return "Connection ID";
    case 0x4c: return "Application Parameters";
    case 0x4d: return "Authenticate Challenge";
    case 0x4e: return "Authenticate Response";
    case 0xcf: return "Creator ID";
    case 0x50: return "WAN UUID";
    case 0x51: return "Object Class";
    case 0x52: return "Session Parameters";
    case 0x93: return "Session Sequence Number";
    case 0x94: return "Action ID";
    case 0x15: return "DestName";
    case 0xd6: return "Permissions";
    case 0x97: return "Single Response Mode";
    case 0x98: return "SRM Parameters";
    case 0x30: return "Img-Handle";  // BIP's user-defined headers
    case 0x71: return "Img-Descriptor";
    default: return nullptr;
  }
}

// The services a Target or Who header names (their 128-bit UUIDs).
const char* obex_target_name(const uint8_t* p, size_t n) {
  static const struct {
    uint8_t uuid[16];
    const char* name;
  } kTargets[] = {
      {{0xf9, 0xec, 0x7b, 0xc4, 0x95, 0x3c, 0x11, 0xd2, 0x98, 0x4e, 0x52, 0x54, 0x00, 0xdc, 0x9e, 0x09}, "File Transfer"},
      {{0x79, 0x61, 0x35, 0xf0, 0xf0, 0xc5, 0x11, 0xd8, 0x09, 0x66, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66}, "Phonebook Access"},
      {{0xbb, 0x58, 0x2b, 0x40, 0x42, 0x0c, 0x11, 0xdb, 0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66}, "Message Access"},
      {{0xbb, 0x58, 0x2b, 0x41, 0x42, 0x0c, 0x11, 0xdb, 0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66}, "Message Notification"},
      {{0x71, 0x63, 0xdd, 0x54, 0x4a, 0x7e, 0x11, 0xe2, 0xb4, 0x7c, 0x00, 0x50, 0xc2, 0x49, 0x00, 0x48}, "BIP Cover Art"},
  };
  if (n != 16) return nullptr;
  for (const auto& t : kTargets)
    if (std::memcmp(t.uuid, p, 16) == 0) return t.name;
  return nullptr;
}

// UTF-16BE text (a Name header), as UTF-8 for the summary; NUL-terminated.
std::string utf16be(const uint8_t* p, size_t n, size_t max = 80) {
  std::string s;
  for (size_t i = 0; i + 1 < n && s.size() < max; i += 2) {
    uint32_t c = be16(p + i);
    if (c == 0) break;
    if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
      c = 0x10000 + ((c - 0xd800) << 10) + (be16(p + i + 2) - 0xdc00);
      i += 2;
    }
    if (c < 0x20) {
      s.push_back('.');
    } else if (c < 0x80) {
      s.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      s.push_back(static_cast<char>(0xc0 | (c >> 6)));
      s.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
      s.push_back(static_cast<char>(0xe0 | (c >> 12)));
      s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      s.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
      s.push_back(static_cast<char>(0xf0 | (c >> 18)));
      s.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
      s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      s.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
  }
  return s;
}

// Does this look like the start of an OBEX packet? The opcode or response code is one the spec
// defines and the packet length covers exactly these bytes (or more, for a packet split over
// several frames). Request vs. response is not known from the bytes alone: both are tried.
bool is_obex(const uint8_t* p, size_t n) {
  if (n < 3) return false;
  const size_t len = be16(p + 1);
  if (len < 3 || len < n) return false;
  if (len > n && n < 64) return false;  // a first fragment is a full frame, not a short tail
  const bool req = obex_opcode_name(p[0]) && ((p[0] & 0x7f) <= 0x07 || p[0] == 0xff);
  const bool rsp = (p[0] & 0x80) && obex_response_name(p[0]);
  return req || rsp;
}

std::string obex(Out& o, const uint8_t* p, size_t n) {
  const uint8_t op = p[0];
  const size_t len = be16(p + 1);
  // 0x80..0x87 and 0xff are requests (Connect, Disconnect, Put/Get final, SetPath, ...); the
  // response codes start at 0x90.
  const bool rsp = (op & 0x7f) >= 0x10 && op != 0xff;
  std::string s = "OBEX ";
  if (rsp) {
    const char* rn = obex_response_name(op);
    s += sf("%s (0x%02x)", rn ? rn : "?", op);
  } else {
    s += obex_opcode_name(op);
    if ((op & 0x80) && op != 0x80 && op != 0x81 && op != 0x85 && op != 0xff) s += " final";
  }
  OPEN("OBEX", s.substr(5), p, n);
  F(rsp ? "Response code" : "Opcode", sf("0x%02x", op) + (op & 0x80 ? " (final)" : ""), p, 1);
  F("Packet length", std::to_string(len) + (len > n ? sf(" (%zu here)", n) : ""), p + 1, 2);
  size_t at = 3;
  // Connect (request and its response) carries version, flags and the maximum packet length;
  // SetPath flags and constants. A response does not say what it answers: a 7-byte prefix whose
  // version is 1.0 is taken to be a Connect response.
  if ((op == 0x80 || (rsp && n >= 7 && p[3] == 0x10)) && n >= 7) {
    F("Version", sf("%u.%u", p[3] >> 4, p[3] & 15), p + 3, 1);
    F("Flags", sf("0x%02x", p[4]), p + 4, 1);
    F("Maximum packet length", std::to_string(be16(p + 5)), p + 5, 2);
    s += sf(" max %u", be16(p + 5));
    at = 7;
  } else if (op == 0x85 && n >= 5) {
    F("Flags", sf("0x%02x%s%s", p[3], p[3] & 1 ? " (backup a level)" : "", p[3] & 2 ? " (don't create)" : ""), p + 3, 1);
    F("Constants", sf("0x%02x", p[4]), p + 4, 1);
    if (p[3] & 1) s += " up";
    at = 5;
  }
  std::string hs;
  while (at < n) {
    const uint8_t hi = p[at];
    const char* hn = obex_header_name(hi);
    const std::string name = hn ? hn : sf("Header 0x%02x", hi);
    size_t hl = 0;
    std::string val;
    if ((hi >> 6) == 0 || (hi >> 6) == 1) {  // length-prefixed: Unicode text or bytes
      if (at + 3 > n) break;
      hl = be16(p + at + 1);
      if (hl < 3) break;
      const uint8_t* v = p + at + 3;
      const size_t vn = std::min(hl, n - at) - 3;
      if ((hi >> 6) == 0) {
        val = "\"" + utf16be(v, vn, 200) + "\"";
      } else if (hi == 0x46 || hi == 0x4a) {
        const char* t = obex_target_name(v, vn);
        val = t ? sf("%s (", t) + hexbytes(v, vn) + ")" : hexbytes(v, vn);
      } else if (hi == 0x42 || hi == 0x44) {
        val = quoted(v, vn, 100);
      } else {
        val = sf("%zu bytes", hl - 3) + (vn ? ": " + hexbytes(v, vn, 32) : "");
      }
      if (hi == 0x01 || hi == 0x42 || hi == 0x46 || hi == 0x4a || hi == 0x15)
        hs += ", " + name + " " + (hi == 0x46 || hi == 0x4a ? (obex_target_name(v, vn) ? obex_target_name(v, vn) : "?") : val);
      if (hi == 0x48 || hi == 0x49) hs += sf(", %s %zu bytes", name.c_str(), hl - 3);
    } else if ((hi >> 6) == 2) {
      if (at + 2 > n) break;
      hl = 2;
      val = sf("0x%02x", p[at + 1]);
      if (hi == 0x97) val += p[at + 1] == 1 ? " (enabled)" : p[at + 1] == 0 ? " (disabled)" : "";
      if (hi == 0x97) hs += ", SRM";
    } else {
      if (at + 5 > n) break;
      hl = 5;
      val = std::to_string(be32(p + at + 1));
      if (hi == 0xcb) val = sf("0x%08x", be32(p + at + 1));
      if (hi == 0xc3) hs += ", Length " + val;
    }
    F(name, val, p + at, std::min(hl, n - at));
    at += hl;
  }
  if (at < n) F("Data", hexbytes(p + at, n - at, 32), p + at, n - at);
  CLOSE();
  return s + hs;
}

// AT commands and result codes of HFP/HSP (HFP 1.9 4.34): the codec IDs named, the rest as text.
std::string at_text(Out& o, const uint8_t* p, size_t n) {
  std::string t = printable(p, n, 120);
  std::string extra;
  auto codec = [](int id) {
    return id == 1 ? std::string("CVSD") : id == 2 ? std::string("mSBC") : id == 3 ? std::string("LC3-SWB") : sf("codec %d", id);
  };
  if (t.rfind("+BCS:", 0) == 0 || t.rfind("AT+BCS=", 0) == 0) {
    extra = codec(std::atoi(t.c_str() + t.find_first_of(":=") + 1));
  } else if (t.rfind("AT+BAC=", 0) == 0) {
    for (size_t at = 7; at < t.size();) {
      extra += (extra.empty() ? "" : ", ") + codec(std::atoi(t.c_str() + at));
      const size_t c = t.find(',', at);
      if (c == std::string::npos) break;
      at = c + 1;
    }
  } else if (t.rfind("AT+VGS=", 0) == 0 || t.rfind("AT+VGM=", 0) == 0 || t.rfind("+VGS", 0) == 0 || t.rfind("+VGM", 0) == 0) {
    const int g = std::atoi(t.c_str() + t.find_first_of(":=") + 1);
    extra = sf("gain %d/15", g);
  } else if (t.rfind("AT+BRSF=", 0) == 0 || t.rfind("+BRSF:", 0) == 0) {
    // HF features in the command, AG features in the result.
    static const char* hf[] = {"EC/NR", "3-way", "CLI", "voice recognition", "remote volume", "enhanced call status",
                               "enhanced call control", "codec negotiation", "HF indicators", "eSCO S4", "enhanced voice recognition",
                               "voice recognition text"};
    static const char* ag[] = {"3-way", "EC/NR", "voice recognition", "in-band ring", "voice tag", "reject call",
                               "enhanced call status", "enhanced call control", "extended error codes", "codec negotiation",
                               "HF indicators", "eSCO S4", "enhanced voice recognition", "voice recognition text"};
    const bool is_ag = t[0] == '+';
    const unsigned v = static_cast<unsigned>(std::strtoul(t.c_str() + t.find_first_of(":=") + 1, nullptr, 10));
    const size_t nb = is_ag ? 14 : 12;
    std::string f;
    for (size_t b = 0; b < nb; ++b)
      if (v & (1u << b)) f += (f.empty() ? "" : ", ") + std::string(is_ag ? ag[b] : hf[b]);
    F(is_ag ? "AG features" : "HF features", sf("0x%x: ", v) + f, p, n);
  }
  F("AT", "\"" + t + "\"" + (extra.empty() ? "" : " (" + extra + ")"), p, n);
  return t + (extra.empty() ? "" : " (" + extra + ")");
}

// ---------------------------------------------------------------------------------------------
// RFCOMM

std::string rfcomm(Out& o, const uint8_t* d, size_t n) {
  if (n < 3) return "RFCOMM (truncated)";
  const uint8_t addr = d[0], ctrl = d[1];
  const uint8_t dlci = addr >> 2;
  const bool pf = ctrl & 0x10;
  const uint8_t ft = ctrl & ~0x10;
  const char* fname = ft == 0x2f   ? "SABM"
                      : ft == 0x63 ? "UA"
                      : ft == 0x0f ? "DM"
                      : ft == 0x43 ? "DISC"
                      : ft == 0xef ? "UIH"
                      : ft == 0x03 ? "UI"
                                   : "?";
  size_t len = d[2] >> 1, hdr = 3;
  if (!(d[2] & 1)) {
    if (n < 4) return "RFCOMM (truncated)";
    len = (d[2] >> 1) | (static_cast<size_t>(d[3]) << 7);
    hdr = 4;
  }
  std::string s = sf("%s dlci %u", fname, dlci);
  OPEN("RFCOMM", s, d, n);
  F("Address", sf("dlci %u (channel %u)%s", dlci, dlci >> 1, addr & 0x02 ? " C/R" : ""), d, 1);
  F("Control", sf("0x%02x (%s%s)", ctrl, fname, pf ? ", P/F" : ""), d + 1, 1);
  F("Length", std::to_string(len), d + 2, hdr - 2);
  // UIH with P/F set on a data channel carries a credit byte (credit-based flow control).
  if (ft == 0xef && pf && dlci != 0 && hdr < n) {
    F("Credits", std::to_string(d[hdr]), d + hdr, 1);
    s += sf(" +%u credits", d[hdr]);
    ++hdr;
  }
  const uint8_t* p = d + hdr;
  const size_t m = std::min(len, n > hdr ? n - hdr - 1 : 0);  // the last byte is the FCS
  if (ft == 0xef && dlci == 0 && m >= 2) {
    // The multiplexer: type and length, then the command.
    const uint8_t type = p[0] >> 2;
    const bool cr = p[0] & 0x02;
    const char* tn = type == 0x20   ? "PN"
                     : type == 0x08 ? "Test"
                     : type == 0x28 ? "FCon"
                     : type == 0x18 ? "FCoff"
                     : type == 0x38 ? "MSC"
                     : type == 0x24 ? "RPN"
                     : type == 0x14 ? "RLS"
                     : type == 0x10 ? "PSC"
                     : type == 0x30 ? "CLD"
                     : type == 0x34 ? "SNC"
                     : type == 0x04 ? "NSC"
                                    : "?";
    F("MCC type", sf("%s (%s)", tn, cr ? "command" : "response"), p, 1);
    s = sf("UIH dlci 0 %s %s", tn, cr ? "cmd" : "rsp");
    const uint8_t* v = p + 2;
    const size_t vn = m - 2;
    if (type == 0x20 && vn >= 8) {
      F("DLCI", std::to_string(v[0] & 0x3f), v, 1);
      F("Frame size", std::to_string(le16(v + 4)), v + 4, 2);
      F("Initial credits", std::to_string(v[7] & 0x07), v + 7, 1);
      s += sf(" dlci %u mtu %u", v[0] & 0x3f, le16(v + 4));
    } else if (type == 0x38 && vn >= 2) {
      F("DLCI", std::to_string(v[0] >> 2), v, 1);
      F("Signals", sf("0x%02x", v[1]), v + 1, 1);
      s += sf(" dlci %u", v[0] >> 2);
    } else if (vn) {
      F("Data", hexbytes(v, vn), v, vn);
    }
  } else if (m) {
    if (mostly_text(p, m)) {
      s += ": " + at_text(o, p, m);
    } else if (is_obex(p, m)) {
      s += ": " + obex(o, p, m);
    } else {
      F("Data", hexbytes(p, m, 64), p, m);
      s += sf(", %zu bytes", m);
    }
  }
  if (hdr + m < n) F("FCS", sf("0x%02x", d[n - 1]), d + n - 1, 1);
  CLOSE();
  return s;
}

// ---------------------------------------------------------------------------------------------
// AVDTP

const char* avdtp_category(uint8_t c) {
  switch (c) {
    case 1: return "Media Transport";
    case 2: return "Reporting";
    case 3: return "Recovery";
    case 4: return "Content Protection";
    case 5: return "Header Compression";
    case 6: return "Multiplexing";
    case 7: return "Media Codec";
    case 8: return "Delay Reporting";
    default: return "?";
  }
}

// A service capability list (GET_CAPABILITIES, SET_CONFIGURATION, ...): each category in the
// tree, the codec with its fields. Returns the summary's part: the codec and the notable extras.
std::string avdtp_caps(Out& o, const uint8_t* p, size_t n) {
  std::string codec, extra;
  while (n >= 2) {
    const uint8_t cat = p[0];
    const size_t cl = p[1];
    if (cl > n - 2) {
      F(avdtp_category(cat), sf("truncated (%zu of %zu bytes)", n - 2, cl), p, n);
      break;
    }
    const uint8_t* v = p + 2;
    if (cat == 7) {
      CodecInfo ci;
      if (decode_media_codec(v, cl, &ci)) {
        codec = ci.name + (ci.config.empty() ? "" : " " + ci.config);
        OPEN("Media Codec", codec, p, 2 + cl);
        static const char* media[] = {"audio", "video", "multimedia"};
        F("Media type", sf("0x%x (%s)", v[0] >> 4, (v[0] >> 4) < 3 ? media[v[0] >> 4] : "?"), v, 1);
        F("Codec type", sf("0x%02x (%s)", v[1], v[1] == 0xff ? "vendor" : ci.name.c_str()), v + 1, 1);
        for (const auto& f : ci.fields) F(f.first, f.second, v + 2, cl - 2);
        CLOSE();
      } else {
        F("Media Codec", hexbytes(v, cl), p, 2 + cl);
      }
    } else if (cat == 4 && cl >= 2) {  // Content Protection: type, then type-specific data
      const uint16_t cp = le16(v);
      const char* cn = cp == 0x0001 ? "DTCP" : cp == 0x0002 ? "SCMS-T" : "?";
      F("Content Protection", sf("0x%04x (%s)", cp, cn) + (cl > 2 ? " " + hexbytes(v + 2, cl - 2) : ""), p, 2 + cl);
      extra += std::string(", ") + cn;
    } else if (cat == 3 && cl >= 3) {
      F("Recovery", sf("type 0x%02x%s, MRWS %u, MNMP %u", v[0], v[0] == 1 ? " (RFC 2733)" : "", v[1], v[2]), p, 2 + cl);
      extra += ", recovery";
    } else if (cat == 5 && cl >= 1) {
      F("Header Compression", sf("0x%02x:%s%s%s", v[0], v[0] & 0x80 ? " back channel" : "",
                                 v[0] & 0x40 ? " media" : "", v[0] & 0x20 ? " recovery" : ""), p, 2 + cl);
    } else {
      F(avdtp_category(cat) + std::string(cat >= 1 && cat <= 8 ? "" : sf(" 0x%02x", cat)),
        cl ? hexbytes(v, cl) : "", p, 2 + cl);
      if (cat == 6) extra += ", multiplexing";
      if (cat == 8) extra += ", delay reporting";
    }
    p += 2 + cl;
    n -= 2 + cl;
  }
  if (codec.empty() && !extra.empty()) return extra.substr(2);
  return codec + extra;
}

std::string avdtp_error_str(uint8_t e) {
  const char* nm = avdtp_error_name(e);
  return nm ? sf("%s (0x%02x)", nm, e) : sf("0x%02x", e);
}

std::string avdtp(Out& o, const uint8_t* d, size_t n) {
  if (n < 2) return "AVDTP (truncated)";
  const uint8_t label = d[0] >> 4, ptype = (d[0] >> 2) & 3, mtype = d[0] & 3;
  static const char* mtypes[] = {"cmd", "general reject", "accept", "reject"};
  static const char* ptypes[] = {"single", "start", "continue", "end"};
  OPEN("AVDTP", "", d, n);
  F("Header", sf("label %u, %s packet, %s", label, ptypes[ptype], mtypes[mtype]), d, 1);
  if (ptype == 2 || ptype == 3) {
    CLOSE();
    return sf("%s packet (label %u)", ptypes[ptype], label);
  }
  const uint8_t* sigp = d + (ptype == 1 ? 2 : 1);
  if (ptype == 1 && n < 3) {
    CLOSE();
    return "AVDTP (truncated)";
  }
  const uint8_t signal = sigp[0] & 0x3f;
  const char* nm = avdtp_signal_name(signal);
  F("Signal", sf("0x%02x (%s)", signal, nm), sigp, 1);
  std::string s = sf("%s %s", nm, mtypes[mtype]);
  if (ptype == 1) {
    F("Packets", std::to_string(d[1]), d + 1, 1);
    CLOSE();
    return s + " (fragmented)";
  }
  const uint8_t* p = d + 2;
  const size_t m = n - 2;
  o.mark(p);
  if (mtype == 0) {
    switch (signal) {
      case 0x03:  // SET_CONFIGURATION
      case 0x05: {
        const size_t caps = signal == 0x03 ? 2 : 1;
        if (m < caps) break;
        F("ACP SEID", std::to_string(p[0] >> 2), p, 1);
        s += sf(" acp %u", p[0] >> 2);
        if (signal == 0x03) {
          F("INT SEID", std::to_string(p[1] >> 2), p + 1, 1);
          s += sf(" int %u", p[1] >> 2);
        }
        const std::string c = avdtp_caps(o, p + caps, m - caps);
        if (!c.empty()) s += ": " + c;
        break;
      }
      case 0x07:  // START
      case 0x09: {  // SUSPEND
        std::string l;
        for (size_t i = 0; i < m; ++i) l += sf(" %u", p[i] >> 2);
        F("ACP SEIDs", l, p, m);
        s += " acp" + l;
        break;
      }
      case 0x0d:
        if (m >= 3) {
          F("ACP SEID", std::to_string(p[0] >> 2), p, 1);
          F("Delay", sf("%.1f ms", be16(p + 1) / 10.0), p + 1, 2);
          s += sf(" acp %u: %.1f ms", p[0] >> 2, be16(p + 1) / 10.0);
        }
        break;
      case 0x01:
        break;
      default:
        if (m >= 1) {
          F("ACP SEID", std::to_string(p[0] >> 2), p, 1);
          s += sf(" acp %u", p[0] >> 2);
        }
        break;
    }
  } else if (mtype == 2) {
    if (signal == 0x01) {
      s += ":";
      for (size_t i = 0; i + 1 < m; i += 2) {
        static const char* media[] = {"audio ", "video ", "multimedia "};
        const std::string sep = sf("%u %s%s%s", p[i] >> 2, (p[i + 1] >> 4) < 3 ? media[p[i + 1] >> 4] : "",
                                   (p[i + 1] & 0x08) ? "sink" : "source", (p[i] & 0x02) ? " (in use)" : "");
        F(sf("SEP %u", p[i] >> 2), sep, p + i, 2);
        s += (i ? ", " : " ") + sep;
      }
    } else if (signal == 0x02 || signal == 0x0c || signal == 0x04) {
      const std::string c = avdtp_caps(o, p, m);
      if (!c.empty()) s += ": " + c;
    }
  } else if (mtype == 3) {
    // SET_CONFIGURATION and RECONFIGURE name the category that failed first, START and SUSPEND
    // the stream endpoint.
    const bool cat_first = signal == 0x03 || signal == 0x05;
    const bool seid_first = signal == 0x07 || signal == 0x09;
    const size_t at = cat_first || seid_first ? 1 : 0;
    if (cat_first && m >= 1) F("Service category", sf("0x%02x (%s)", p[0], avdtp_category(p[0])), p, 1);
    if (seid_first && m >= 1) F("ACP SEID", std::to_string(p[0] >> 2), p, 1);
    if (m > at) {
      F("Error", avdtp_error_str(p[at]), p + at, 1);
      s += ": " + avdtp_error_str(p[at]);
      if (cat_first && m >= 1) s += sf(" (%s)", avdtp_category(p[0]));
      if (seid_first && m >= 1) s += sf(" (acp %u)", p[0] >> 2);
    }
  }
  rest(o, p, m);
  CLOSE();
  return s;
}

// The transport channel: RTP, then the codec's payload. SBC is recognised by its syncword after
// the one-byte media payload header, so no stream state is needed for it.
std::string rtp(Out& o, const uint8_t* d, size_t n) {
  if (n < 12 || (d[0] >> 6) != 2) {
    F("Media data", hexbytes(d, n, 32), d, n);
    return sf("media data, %zu bytes", n);
  }
  const uint16_t seq = be16(d + 2);
  const uint32_t ts = be32(d + 4);
  OPEN("RTP", sf("seq %u", seq), d, n);
  F("Version", "2", d, 1);
  F("Payload type", std::to_string(d[1] & 0x7f), d + 1, 1);
  if (d[1] & 0x80) F("Marker", "1", d + 1, 1);
  F("Sequence number", std::to_string(seq), d + 2, 2);
  F("Timestamp", std::to_string(ts), d + 4, 4);
  F("SSRC", sf("0x%08x", be32(d + 8)), d + 8, 4);
  size_t off = 12 + 4u * (d[0] & 0x0f);
  if ((d[0] & 0x10) && off + 4 <= n) off += 4 + 4u * be16(d + off + 2);
  std::string s = sf("RTP seq %u ts %u", seq, ts);
  CLOSE();
  if (off + 2 <= n && d[off + 1] == 0x9c) {
    const uint8_t hdr = d[off];
    const uint8_t frames = hdr & 0x0f;
    OPEN("SBC", sf("%u frames", frames), d + off, n - off);
    F("Frames", std::to_string(frames), d + off, 1);
    if (hdr & 0x80) F("Fragmented", hdr & 0x40 ? "start" : hdr & 0x20 ? "last" : "yes", d + off, 1);
    s += sf(", SBC %u frames", frames);
    const uint8_t* f = d + off + 1;
    if (off + 1 + 4 <= n) {
      static const int rates[] = {16000, 32000, 44100, 48000};
      static const char* modes[] = {"mono", "dual channel", "stereo", "joint stereo"};
      const uint8_t b1 = f[1];
      F("Sampling frequency", sf("%d Hz", rates[b1 >> 6]), f + 1, 1);
      F("Blocks", std::to_string(4 * (((b1 >> 4) & 3) + 1)), f + 1, 1);
      F("Channel mode", modes[(b1 >> 2) & 3], f + 1, 1);
      F("Allocation", b1 & 0x02 ? "SNR" : "loudness", f + 1, 1);
      F("Subbands", b1 & 0x01 ? "8" : "4", f + 1, 1);
      F("Bitpool", std::to_string(f[2]), f + 2, 1);
      s += sf(", bitpool %u", f[2]);
    }
    CLOSE();
  } else if (off < n) {
    F("Payload", hexbytes(d + off, n - off, 32), d + off, n - off);
    s += sf(", %zu bytes payload", n - off);
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// AVCTP / AVRCP

const char* avrcp_pdu_name(uint8_t id) {
  switch (id) {
    case 0x10: return "GetCapabilities";
    case 0x11: return "ListPlayerApplicationSettingAttributes";
    case 0x12: return "ListPlayerApplicationSettingValues";
    case 0x13: return "GetCurrentPlayerApplicationSettingValue";
    case 0x14: return "SetPlayerApplicationSettingValue";
    case 0x15: return "GetPlayerApplicationSettingAttributeText";
    case 0x16: return "GetPlayerApplicationSettingValueText";
    case 0x17: return "InformDisplayableCharacterSet";
    case 0x18: return "InformBatteryStatusOfCT";
    case 0x20: return "GetElementAttributes";
    case 0x30: return "GetPlayStatus";
    case 0x31: return "RegisterNotification";
    case 0x40: return "RequestContinuingResponse";
    case 0x41: return "AbortContinuingResponse";
    case 0x50: return "SetAbsoluteVolume";
    case 0x60: return "SetAddressedPlayer";
    case 0x70: return "SetBrowsedPlayer";
    case 0x71: return "GetFolderItems";
    case 0x72: return "ChangePath";
    case 0x73: return "GetItemAttributes";
    case 0x74: return "PlayItem";
    case 0x75: return "GetTotalNumberOfItems";
    case 0x80: return "Search";
    case 0x90: return "AddToNowPlaying";
    case 0xa0: return "GeneralReject";
    default: return nullptr;
  }
}

const char* avrcp_event_name(uint8_t e) {
  switch (e) {
    case 0x01: return "PLAYBACK_STATUS_CHANGED";
    case 0x02: return "TRACK_CHANGED";
    case 0x03: return "TRACK_REACHED_END";
    case 0x04: return "TRACK_REACHED_START";
    case 0x05: return "PLAYBACK_POS_CHANGED";
    case 0x06: return "BATT_STATUS_CHANGED";
    case 0x07: return "SYSTEM_STATUS_CHANGED";
    case 0x08: return "PLAYER_APPLICATION_SETTING_CHANGED";
    case 0x09: return "NOW_PLAYING_CONTENT_CHANGED";
    case 0x0a: return "AVAILABLE_PLAYERS_CHANGED";
    case 0x0b: return "ADDRESSED_PLAYER_CHANGED";
    case 0x0c: return "UIDS_CHANGED";
    case 0x0d: return "VOLUME_CHANGED";
    default: return "?";
  }
}

// The status a rejected command or a browsing response carries (AVRCP 1.6 6.15.3).
const char* avrcp_status_name(uint8_t s) {
  switch (s) {
    case 0x00: return "Invalid Command";
    case 0x01: return "Invalid Parameter";
    case 0x02: return "Parameter Content Error";
    case 0x03: return "Internal Error";
    case 0x04: return "Success";
    case 0x05: return "UID Changed";
    case 0x07: return "Invalid Direction";
    case 0x08: return "Not a Directory";
    case 0x09: return "Does Not Exist";
    case 0x0a: return "Invalid Scope";
    case 0x0b: return "Range Out of Bounds";
    case 0x0c: return "Folder Item is not Playable";
    case 0x0d: return "Media in Use";
    case 0x0e: return "Now Playing List Full";
    case 0x0f: return "Search Not Supported";
    case 0x10: return "Search in Progress";
    case 0x11: return "Invalid Player Id";
    case 0x12: return "Player Not Browsable";
    case 0x13: return "Player Not Addressed";
    case 0x14: return "No valid Search Results";
    case 0x15: return "No available players";
    case 0x16: return "Addressed Player Changed";
    default: return "?";
  }
}

std::string avrcp_status_str(uint8_t s) { return sf("%s (0x%02x)", avrcp_status_name(s), s); }

// Media attributes (AVRCP 1.6 26 Appendix E).
const char* avrcp_attr_name(uint32_t a) {
  switch (a) {
    case 0x1: return "Title";
    case 0x2: return "Artist";
    case 0x3: return "Album";
    case 0x4: return "Track number";
    case 0x5: return "Total tracks";
    case 0x6: return "Genre";
    case 0x7: return "Playing time";
    case 0x8: return "Cover art handle";
    default: return nullptr;
  }
}

std::string avrcp_attr_str(uint32_t a) {
  const char* nm = avrcp_attr_name(a);
  return nm ? std::string(nm) : sf("attribute 0x%08x", a);
}

// Player application settings (AVRCP 1.6 Appendix F) and their values.
const char* avrcp_setting_name(uint8_t a) {
  switch (a) {
    case 0x01: return "Equalizer";
    case 0x02: return "Repeat";
    case 0x03: return "Shuffle";
    case 0x04: return "Scan";
    default: return nullptr;
  }
}

std::string avrcp_setting_str(uint8_t a) {
  const char* nm = avrcp_setting_name(a);
  return nm ? std::string(nm) : sf("setting 0x%02x", a);
}

std::string avrcp_setting_value(uint8_t a, uint8_t v) {
  static const char* eq[] = {"off", "on"};
  static const char* repeat[] = {"off", "single track", "all tracks", "group"};
  static const char* shuffle[] = {"off", "all tracks", "group"};
  if (a == 1 && v >= 1 && v <= 2) return eq[v - 1];
  if (a == 2 && v >= 1 && v <= 4) return repeat[v - 1];
  if ((a == 3 || a == 4) && v >= 1 && v <= 3) return shuffle[v - 1];
  return sf("0x%02x", v);
}

const char* play_status_name(uint8_t s) {
  switch (s) {
    case 0x00: return "stopped";
    case 0x01: return "playing";
    case 0x02: return "paused";
    case 0x03: return "forward seek";
    case 0x04: return "reverse seek";
    case 0xff: return "error";
    default: return "?";
  }
}

const char* avrcp_scope_name(uint8_t s) {
  switch (s) {
    case 0: return "Media Player List";
    case 1: return "Media Player Virtual Filesystem";
    case 2: return "Search";
    case 3: return "Now Playing";
    default: return "?";
  }
}

// A position or length in ms as m:ss.mmm; 0xffffffff is "not supported" (or no track selected).
std::string ms_str(uint32_t ms) {
  if (ms == 0xffffffff) return "n/a";
  return sf("%u:%02u.%03u", ms / 60000, (ms / 1000) % 60, ms % 1000);
}

std::string uid_str(const uint8_t* p) {
  const uint64_t v = (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
  if (v == 0) return "0";
  if (v == ~0ull) return "0xffffffffffffffff";
  return sf("0x%016llx", static_cast<unsigned long long>(v));
}

// A list of media attribute values (GetElementAttributes and GetItemAttributes responses, a media
// element item): ID (4), character set (2), length (2), value. The summary names the attributes
// people look for; the tree has all of them. Stops at the end of the bytes (a fragmented response
// continues in the next packet). Returns the bytes used.
size_t avrcp_attr_values(Out& o, const uint8_t* p, size_t n, unsigned count, std::string* sum) {
  size_t at = 0;
  for (unsigned i = 0; i < count && at + 8 <= n; ++i) {
    const uint32_t id = be32(p + at);
    const uint16_t cs = be16(p + at + 4);
    const size_t len = be16(p + at + 6);
    const uint8_t* v = p + at + 8;
    const size_t vn = std::min(len, n - at - 8);
    const char* nm = avrcp_attr_name(id);
    std::string val;
    if (id == 0x7 && vn == len && len && len < 12) {  // playing time: ms as decimal text
      const std::string t = printable(v, vn);
      const uint32_t ms = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 10));
      val = t + " ms (" + ms_str(ms) + ")";
      if (!sum->empty()) *sum += ", ";
      *sum += "Time " + ms_str(ms).substr(0, ms_str(ms).size() - 4);
    } else if ((id == 0x4 || id == 0x5) && vn == len) {  // track number, total: decimal text
      val = printable(v, vn, 12);
      if (vn) {
        if (!sum->empty()) *sum += ", ";
        *sum += (id == 4 ? "Track " : "Tracks ") + val;
      }
    } else {
      val = quoted(v, vn, 200) + (vn < len ? sf(" (%zu of %zu bytes)", vn, len) : "");
      if (nm && id <= 0x6 && (vn || id == 0x1)) {
        if (!sum->empty()) *sum += ", ";
        static const char* shortn[] = {"", "Title", "Artist", "Album", "Track", "Tracks", "Genre"};
        *sum += shortn[id] + std::string(" ") + (id == 4 || id == 5 ? printable(v, vn, 12) : quoted(v, vn));
      }
    }
    if (cs != 0x006a) val += sf(" (charset 0x%04x)", cs);
    F(nm ? nm : sf("Attribute 0x%08x", id), val, p + at, 8 + vn);
    at += 8 + vn;
  }
  return at;
}

// A RegisterNotification response's event data: what changed, as the summary's words.
std::string avrcp_event_data(Out& o, uint8_t ev, const uint8_t* v, size_t n) {
  switch (ev) {
    case 0x01:
      if (n < 1) break;
      F("Play status", sf("0x%02x (%s)", v[0], play_status_name(v[0])), v, 1);
      return play_status_name(v[0]);
    case 0x02:
      if (n < 8) break;
      F("Identifier", uid_str(v) + (be32(v) == 0 && be32(v + 4) == 0 ? " (a track is selected)"
                                    : be32(v) == 0xffffffff && be32(v + 4) == 0xffffffff ? " (no track selected)" : ""), v, 8);
      return be32(v) == 0xffffffff && be32(v + 4) == 0xffffffff ? "no track" : "track " + uid_str(v);
    case 0x05:
      if (n < 4) break;
      F("Position", sf("%u ms (", be32(v)) + ms_str(be32(v)) + ")", v, 4);
      return ms_str(be32(v));
    case 0x06: {
      if (n < 1) break;
      static const char* batt[] = {"normal", "warning", "critical", "external", "full charge"};
      const char* b = v[0] < 5 ? batt[v[0]] : "?";
      F("Battery status", sf("0x%02x (%s)", v[0], b), v, 1);
      return b;
    }
    case 0x07: {
      if (n < 1) break;
      static const char* sys[] = {"power on", "power off", "unplugged"};
      const char* st = v[0] < 3 ? sys[v[0]] : "?";
      F("System status", sf("0x%02x (%s)", v[0], st), v, 1);
      return st;
    }
    case 0x08: {
      if (n < 1) break;
      std::string s;
      F("Number of settings", std::to_string(v[0]), v, 1);
      for (size_t i = 0; i < v[0] && 2 + 2 * i <= n - 1; ++i) {
        const uint8_t a = v[1 + 2 * i], val = v[2 + 2 * i];
        F(avrcp_setting_str(a), avrcp_setting_value(a, val), v + 1 + 2 * i, 2);
        s += (s.empty() ? "" : ", ") + avrcp_setting_str(a) + " " + avrcp_setting_value(a, val);
      }
      return s;
    }
    case 0x0b:
      if (n < 4) break;
      F("Player ID", std::to_string(be16(v)), v, 2);
      F("UID counter", std::to_string(be16(v + 2)), v + 2, 2);
      return sf("player %u, uid counter %u", be16(v), be16(v + 2));
    case 0x0c:
      if (n < 2) break;
      F("UID counter", std::to_string(be16(v)), v, 2);
      return sf("uid counter %u", be16(v));
    case 0x0d:
      if (n < 1) break;
      F("Volume", sf("%u (%u%%)", v[0] & 0x7f, ((v[0] & 0x7f) * 100 + 63) / 127), v, 1);
      return sf("%u (%u%%)", v[0] & 0x7f, ((v[0] & 0x7f) * 100 + 63) / 127);
    default:
      break;
  }
  return "";
}

// The parameters of a vendor-dependent (AV/C control channel) AVRCP PDU. Returns the summary's
// detail, without the PDU name.
std::string avrcp_vendor_params(Out& o, uint8_t pdu, bool rsp, uint8_t ctype, const uint8_t* v, size_t n) {
  if (rsp && ctype == 0xa && n >= 1) {  // REJECTED: an error status, whatever the PDU
    F("Status", avrcp_status_str(v[0]), v, 1);
    return avrcp_status_str(v[0]);
  }
  std::string s;
  switch (pdu) {
    case 0x10:  // GetCapabilities
      if (n < 1) break;
      F("Capability", sf("0x%02x (%s)", v[0], v[0] == 2 ? "company IDs" : v[0] == 3 ? "events supported" : "?"), v, 1);
      s = v[0] == 2 ? "company IDs" : v[0] == 3 ? "events" : sf("capability 0x%02x", v[0]);
      if (rsp && n >= 2) {
        F("Count", std::to_string(v[1]), v + 1, 1);
        const size_t w = v[0] == 2 ? 3 : 1;
        for (size_t i = 0; i < v[1] && 2 + (i + 1) * w <= n; ++i) {
          const uint8_t* q = v + 2 + i * w;
          if (w == 3) {
            const uint32_t c = (q[0] << 16) | (q[1] << 8) | q[2];
            F("Company ID", sf("0x%06x%s", c, c == 0x001958 ? " (Bluetooth SIG)" : ""), q, 3);
            s += sf(" 0x%06x", c);
          } else {
            F("Event", sf("0x%02x (%s)", q[0], avrcp_event_name(q[0])), q, 1);
            s += std::string(" ") + avrcp_event_name(q[0]);
          }
        }
      }
      break;
    case 0x11:  // ListPlayerApplicationSettingAttributes
      if (!rsp || n < 1) break;
      F("Count", std::to_string(v[0]), v, 1);
      for (size_t i = 0; i < v[0] && 1 + i < n; ++i) {
        F("Setting", sf("0x%02x (%s)", v[1 + i], avrcp_setting_str(v[1 + i]).c_str()), v + 1 + i, 1);
        s += (s.empty() ? "" : ", ") + avrcp_setting_str(v[1 + i]);
      }
      break;
    case 0x12:  // ListPlayerApplicationSettingValues
      if (n < 1) break;
      if (!rsp) {
        F("Setting", sf("0x%02x (%s)", v[0], avrcp_setting_str(v[0]).c_str()), v, 1);
        s = avrcp_setting_str(v[0]);
      } else {
        F("Count", std::to_string(v[0]), v, 1);
        s = "values";
        for (size_t i = 0; i < v[0] && 1 + i < n; ++i) {
          F("Value", sf("0x%02x", v[1 + i]), v + 1 + i, 1);
          s += sf(" %u", v[1 + i]);
        }
      }
      break;
    case 0x13:  // GetCurrentPlayerApplicationSettingValue
    case 0x14:  // SetPlayerApplicationSettingValue
      if (n < 1 || (pdu == 0x14 && rsp)) break;
      F("Count", std::to_string(v[0]), v, 1);
      if (pdu == 0x13 && !rsp) {
        for (size_t i = 0; i < v[0] && 1 + i < n; ++i) {
          F("Setting", sf("0x%02x (%s)", v[1 + i], avrcp_setting_str(v[1 + i]).c_str()), v + 1 + i, 1);
          s += (s.empty() ? "" : ", ") + avrcp_setting_str(v[1 + i]);
        }
      } else {
        for (size_t i = 0; i < v[0] && 3 + 2 * i <= n; ++i) {
          const uint8_t a = v[1 + 2 * i], val = v[2 + 2 * i];
          F(avrcp_setting_str(a), avrcp_setting_value(a, val), v + 1 + 2 * i, 2);
          s += (s.empty() ? "" : ", ") + avrcp_setting_str(a) + " " + avrcp_setting_value(a, val);
        }
      }
      break;
    case 0x15:  // GetPlayerApplicationSettingAttributeText
    case 0x16: {  // GetPlayerApplicationSettingValueText
      if (n < 1) break;
      if (!rsp) {
        size_t at = 0;
        if (pdu == 0x16) {
          F("Setting", sf("0x%02x (%s)", v[0], avrcp_setting_str(v[0]).c_str()), v, 1);
          s = avrcp_setting_str(v[0]);
          at = 1;
        }
        if (at < n) {
          F("Count", std::to_string(v[at]), v + at, 1);
          std::string ids;
          for (size_t i = 0; i < v[at] && at + 1 + i < n; ++i) ids += sf(" 0x%02x", v[at + 1 + i]);
          if (!ids.empty()) F(pdu == 0x15 ? "Settings" : "Values", ids.substr(1), v + at + 1, n - at - 1);
        }
        break;
      }
      F("Count", std::to_string(v[0]), v, 1);
      size_t at = 1;
      for (size_t i = 0; i < v[0] && at + 4 <= n; ++i) {
        const size_t len = std::min<size_t>(v[at + 3], n - at - 4);
        const std::string t = quoted(v + at + 4, len);
        F(sf("0x%02x", v[at]), t, v + at, 4 + len);
        s += (s.empty() ? "" : ", ") + sf("0x%02x ", v[at]) + t;
        at += 4 + len;
      }
      break;
    }
    case 0x17:  // InformDisplayableCharacterSet
      if (n < 1) break;
      F("Count", std::to_string(v[0]), v, 1);
      for (size_t i = 0; i < v[0] && 3 + 2 * i <= n; ++i) {
        const uint16_t cs = be16(v + 1 + 2 * i);
        F("Character set", sf("0x%04x%s", cs, cs == 0x006a ? " (UTF-8)" : ""), v + 1 + 2 * i, 2);
        s += (s.empty() ? "" : ", ") + (cs == 0x006a ? std::string("UTF-8") : sf("0x%04x", cs));
      }
      break;
    case 0x18: {  // InformBatteryStatusOfCT
      if (n < 1 || rsp) break;
      static const char* batt[] = {"normal", "warning", "critical", "external", "full charge"};
      s = v[0] < 5 ? batt[v[0]] : "?";
      F("Battery status", sf("0x%02x (%s)", v[0], s.c_str()), v, 1);
      break;
    }
    case 0x20:  // GetElementAttributes
      if (!rsp) {
        if (n < 9) break;
        F("Identifier", uid_str(v) + (be32(v) == 0 && be32(v + 4) == 0 ? " (playing)" : ""), v, 8);
        F("Count", v[8] ? std::to_string(v[8]) : "0 (all)", v + 8, 1);
        for (size_t i = 0; i < v[8] && 9 + 4 * (i + 1) <= n; ++i) {
          const uint32_t a = be32(v + 9 + 4 * i);
          F("Attribute", sf("0x%08x (%s)", a, avrcp_attr_str(a).c_str()), v + 9 + 4 * i, 4);
          s += (s.empty() ? "" : ", ") + avrcp_attr_str(a);
        }
        if (!v[8]) s = "all";
      } else {
        if (n < 1) break;
        F("Count", std::to_string(v[0]), v, 1);
        avrcp_attr_values(o, v + 1, n - 1, v[0], &s);
      }
      break;
    case 0x30:  // GetPlayStatus
      if (!rsp || n < 9) break;
      F("Song length", sf("%u ms (", be32(v)) + ms_str(be32(v)) + ")", v, 4);
      F("Song position", sf("%u ms (", be32(v + 4)) + ms_str(be32(v + 4)) + ")", v + 4, 4);
      F("Play status", sf("0x%02x (%s)", v[8], play_status_name(v[8])), v + 8, 1);
      s = std::string(play_status_name(v[8])) + " " + ms_str(be32(v + 4)) + " / " + ms_str(be32(v));
      break;
    case 0x31:  // RegisterNotification
      if (n < 1) break;
      F("Event", sf("0x%02x (%s)", v[0], avrcp_event_name(v[0])), v, 1);
      s = avrcp_event_name(v[0]);
      if (!rsp) {
        if (n >= 5 && v[0] == 0x05) F("Playback interval", sf("%u s", be32(v + 1)), v + 1, 4);
        else if (n >= 5) F("Interval", std::to_string(be32(v + 1)), v + 1, 4);
      } else {
        const std::string d = avrcp_event_data(o, v[0], v + 1, n - 1);
        if (!d.empty()) s += ": " + d;
      }
      break;
    case 0x40:  // RequestContinuingResponse
    case 0x41:  // AbortContinuingResponse
      if (n < 1) break;
      F("Continuing PDU", sf("0x%02x (%s)", v[0], avrcp_pdu_name(v[0]) ? avrcp_pdu_name(v[0]) : "?"), v, 1);
      s = avrcp_pdu_name(v[0]) ? avrcp_pdu_name(v[0]) : sf("0x%02x", v[0]);
      break;
    case 0x50:  // SetAbsoluteVolume
      if (n < 1) break;
      F("Volume", sf("%u (%u%%)", v[0] & 0x7f, ((v[0] & 0x7f) * 100 + 63) / 127), v, 1);
      s = sf("%u (%u%%)", v[0] & 0x7f, ((v[0] & 0x7f) * 100 + 63) / 127);
      break;
    case 0x60:  // SetAddressedPlayer
      if (!rsp && n >= 2) {
        F("Player ID", std::to_string(be16(v)), v, 2);
        s = sf("player %u", be16(v));
      } else if (rsp && n >= 1) {
        F("Status", avrcp_status_str(v[0]), v, 1);
        s = avrcp_status_name(v[0]);
      }
      break;
    case 0x74:  // PlayItem
    case 0x90:  // AddToNowPlaying
      if (!rsp && n >= 11) {
        F("Scope", sf("0x%02x (%s)", v[0], avrcp_scope_name(v[0])), v, 1);
        F("UID", uid_str(v + 1), v + 1, 8);
        F("UID counter", std::to_string(be16(v + 9)), v + 9, 2);
        s = std::string(avrcp_scope_name(v[0])) + " " + uid_str(v + 1);
      } else if (rsp && n >= 1) {
        F("Status", avrcp_status_str(v[0]), v, 1);
        s = avrcp_status_name(v[0]);
      }
      break;
    default:
      break;
  }
  return s;
}

// One item of a GetFolderItems response; returns its name for the summary.
std::string avrcp_item(Out& o, const uint8_t* p, size_t n) {
  const uint8_t type = p[0];
  const uint8_t* v = p + 3;
  const size_t vn = n - 3;
  std::string name;
  if (type == 0x01 && vn >= 28) {  // media player
    const size_t len = std::min<size_t>(be16(v + 26), vn - 28);
    name = quoted(v + 28, len);
    OPEN("Media player", name, p, n);
    F("Player ID", std::to_string(be16(v)), v, 2);
    F("Major type", sf("0x%02x%s%s%s%s", v[2], v[2] & 1 ? " audio" : "", v[2] & 2 ? " video" : "",
                       v[2] & 4 ? " broadcasting audio" : "", v[2] & 8 ? " broadcasting video" : ""), v + 2, 1);
    F("Subtype", sf("0x%08x%s%s", be32(v + 3), be32(v + 3) & 1 ? " audio book" : "", be32(v + 3) & 2 ? " podcast" : ""), v + 3, 4);
    F("Play status", sf("0x%02x (%s)", v[7], play_status_name(v[7])), v + 7, 1);
    F("Features", hexbytes(v + 8, 16), v + 8, 16);
    F("Character set", sf("0x%04x", be16(v + 24)), v + 24, 2);
    F("Name", name, v + 28, len);
    name = "player " + name;
  } else if (type == 0x02 && vn >= 14) {  // folder
    static const char* ftypes[] = {"mixed", "titles", "albums", "artists", "genres", "playlists", "years"};
    const size_t len = std::min<size_t>(be16(v + 12), vn - 14);
    name = quoted(v + 14, len);
    OPEN("Folder", name, p, n);
    F("UID", uid_str(v), v, 8);
    F("Folder type", sf("0x%02x (%s)", v[8], v[8] < 7 ? ftypes[v[8]] : "?"), v + 8, 1);
    F("Playable", v[9] ? "yes" : "no", v + 9, 1);
    F("Character set", sf("0x%04x", be16(v + 10)), v + 10, 2);
    F("Name", name, v + 14, len);
    name = "folder " + name;
  } else if (type == 0x03 && vn >= 13) {  // media element
    const size_t len = std::min<size_t>(be16(v + 11), vn - 13);
    name = quoted(v + 13, len);
    OPEN("Media element", name, p, n);
    F("UID", uid_str(v), v, 8);
    F("Media type", v[8] == 0 ? "audio" : v[8] == 1 ? "video" : sf("0x%02x", v[8]), v + 8, 1);
    F("Character set", sf("0x%04x", be16(v + 9)), v + 9, 2);
    F("Name", name, v + 13, len);
    if (13 + len < vn) {
      const uint8_t* a = v + 13 + len;
      std::string unused;
      OPEN("Attributes", std::to_string(a[0]), a, vn - 13 - len);
      avrcp_attr_values(o, a + 1, vn - 14 - len, a[0], &unused);
      CLOSE();
    }
  } else {
    OPEN("Item", sf("type 0x%02x", type), p, n);
    if (vn) F("Data", hexbytes(v, vn, 64), v, vn);
  }
  CLOSE();
  return name;
}

// A browsing channel PDU (AVRCP 1.6 6.10.4, 6.11).
std::string avrcp_browsing(Out& o, const uint8_t* p, size_t m, bool rsp) {
  const uint8_t pdu = p[0];
  const char* nm = avrcp_pdu_name(pdu);
  OPEN("AVRCP browsing", nm ? nm : "?", p, m);
  F("PDU", sf("0x%02x (%s)", pdu, nm ? nm : "?"), p, 1);
  F("Parameter length", std::to_string(be16(p + 1)), p + 1, 2);
  const uint8_t* v = p + 3;
  const size_t n = std::min<size_t>(m - 3, be16(p + 1));
  o.mark(v);
  std::string s = std::string(nm ? nm : sf("PDU 0x%02x", pdu).c_str()) + (rsp ? " rsp" : "");
  std::string d;
  if (pdu == 0xa0 && n >= 1) {
    F("Reason", avrcp_status_str(v[0]), v, 1);
    d = avrcp_status_name(v[0]);
  } else if (rsp && n >= 1 && v[0] != 0x04) {  // every response starts with its status
    F("Status", avrcp_status_str(v[0]), v, 1);
    d = avrcp_status_name(v[0]);
  } else if (!rsp) {
    switch (pdu) {
      case 0x70:  // SetBrowsedPlayer
        if (n < 2) break;
        F("Player ID", std::to_string(be16(v)), v, 2);
        d = sf("player %u", be16(v));
        break;
      case 0x71:  // GetFolderItems
        if (n < 10) break;
        F("Scope", sf("0x%02x (%s)", v[0], avrcp_scope_name(v[0])), v, 1);
        F("Start item", std::to_string(be32(v + 1)), v + 1, 4);
        F("End item", std::to_string(be32(v + 5)), v + 5, 4);
        F("Attribute count", v[9] == 0 ? "0 (all)" : v[9] == 0xff ? "0xff (none)" : std::to_string(v[9]), v + 9, 1);
        if (v[9] != 0xff) {
          for (size_t i = 0; i < v[9] && 10 + 4 * (i + 1) <= n; ++i) {
            const uint32_t a = be32(v + 10 + 4 * i);
            F("Attribute", sf("0x%08x (%s)", a, avrcp_attr_str(a).c_str()), v + 10 + 4 * i, 4);
          }
        }
        d = sf("%s %u-%u", avrcp_scope_name(v[0]), be32(v + 1), be32(v + 5));
        break;
      case 0x72:  // ChangePath
        if (n < 11) break;
        F("UID counter", std::to_string(be16(v)), v, 2);
        F("Direction", v[2] == 0 ? "0 (folder up)" : v[2] == 1 ? "1 (folder down)" : sf("0x%02x", v[2]), v + 2, 1);
        F("Folder UID", uid_str(v + 3), v + 3, 8);
        d = v[2] == 0 ? "up" : "down " + uid_str(v + 3);
        break;
      case 0x73:  // GetItemAttributes
        if (n < 12) break;
        F("Scope", sf("0x%02x (%s)", v[0], avrcp_scope_name(v[0])), v, 1);
        F("UID", uid_str(v + 1), v + 1, 8);
        F("UID counter", std::to_string(be16(v + 9)), v + 9, 2);
        F("Count", v[11] ? std::to_string(v[11]) : "0 (all)", v + 11, 1);
        for (size_t i = 0; i < v[11] && 12 + 4 * (i + 1) <= n; ++i) {
          const uint32_t a = be32(v + 12 + 4 * i);
          F("Attribute", sf("0x%08x (%s)", a, avrcp_attr_str(a).c_str()), v + 12 + 4 * i, 4);
        }
        d = std::string(avrcp_scope_name(v[0])) + " " + uid_str(v + 1);
        break;
      case 0x75:  // GetTotalNumberOfItems
        if (n < 1) break;
        F("Scope", sf("0x%02x (%s)", v[0], avrcp_scope_name(v[0])), v, 1);
        d = avrcp_scope_name(v[0]);
        break;
      case 0x80:  // Search
        if (n < 4) break;
        F("Character set", sf("0x%04x", be16(v)), v, 2);
        F("Search string", quoted(v + 4, std::min<size_t>(be16(v + 2), n - 4), 200), v + 4, n - 4);
        d = quoted(v + 4, std::min<size_t>(be16(v + 2), n - 4));
        break;
      default:
        break;
    }
  } else if (n >= 1) {
    F("Status", avrcp_status_str(v[0]), v, 1);
    switch (pdu) {
      case 0x70:  // SetBrowsedPlayer: the browsed folder's path
        if (n < 10) break;
        F("UID counter", std::to_string(be16(v + 1)), v + 1, 2);
        F("Number of items", std::to_string(be32(v + 3)), v + 3, 4);
        F("Character set", sf("0x%04x", be16(v + 7)), v + 7, 2);
        F("Folder depth", std::to_string(v[9]), v + 9, 1);
        {
          std::string path;
          size_t at = 10;
          for (unsigned i = 0; i < v[9] && at + 2 <= n; ++i) {
            const size_t len = std::min<size_t>(be16(v + at), n - at - 2);
            F("Folder", quoted(v + at + 2, len), v + at, 2 + len);
            path += "/" + printable(v + at + 2, len, 40);
            at += 2 + len;
          }
          d = sf("%u items in ", be32(v + 3)) + (path.empty() ? "/" : path);
        }
        break;
      case 0x71:  // GetFolderItems
        if (n < 5) break;
        F("UID counter", std::to_string(be16(v + 1)), v + 1, 2);
        F("Number of items", std::to_string(be16(v + 3)), v + 3, 2);
        d = sf("%u items", be16(v + 3));
        {
          size_t at = 5;
          for (unsigned i = 0; i < be16(v + 3) && at + 3 <= n; ++i) {
            const size_t len = std::min<size_t>(be16(v + at + 1), n - at - 3);
            const std::string nm2 = avrcp_item(o, v + at, 3 + len);
            if (i < 4 && !nm2.empty()) d += (i ? ", " : ": ") + nm2;
            else if (i == 4) d += ", …";
            at += 3 + len;
          }
        }
        break;
      case 0x72:  // ChangePath
        if (n < 5) break;
        F("Number of items", std::to_string(be32(v + 1)), v + 1, 4);
        d = sf("%u items", be32(v + 1));
        break;
      case 0x73:  // GetItemAttributes
        if (n < 2) break;
        F("Count", std::to_string(v[1]), v + 1, 1);
        avrcp_attr_values(o, v + 2, n - 2, v[1], &d);
        break;
      case 0x75:  // GetTotalNumberOfItems
      case 0x80:  // Search
        if (n < 7) break;
        F("UID counter", std::to_string(be16(v + 1)), v + 1, 2);
        F("Number of items", std::to_string(be32(v + 3)), v + 3, 4);
        d = sf("%u items", be32(v + 3));
        break;
      default:
        break;
    }
  }
  rest(o, v, n);
  CLOSE();
  return d.empty() ? s : s + ": " + d;
}

const char* avc_ctype_name(uint8_t c) {
  switch (c) {
    case 0x0: return "CONTROL";
    case 0x1: return "STATUS";
    case 0x2: return "SPECIFIC_INQUIRY";
    case 0x3: return "NOTIFY";
    case 0x4: return "GENERAL_INQUIRY";
    case 0x8: return "NOT_IMPLEMENTED";
    case 0x9: return "ACCEPTED";
    case 0xa: return "REJECTED";
    case 0xb: return "IN_TRANSITION";
    case 0xc: return "STABLE";
    case 0xd: return "CHANGED";
    case 0xf: return "INTERIM";
    default: return "?";
  }
}

// AV/C panel subunit operations (AV/C Panel Subunit 9.4, AVRCP 1.6 Table 9.21).
const char* passthrough_name(uint8_t op) {
  static const char* digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  if (op >= 0x20 && op <= 0x29) return digits[op - 0x20];
  switch (op) {
    case 0x00: return "SELECT";
    case 0x01: return "UP";
    case 0x02: return "DOWN";
    case 0x03: return "LEFT";
    case 0x04: return "RIGHT";
    case 0x05: return "RIGHT_UP";
    case 0x06: return "RIGHT_DOWN";
    case 0x07: return "LEFT_UP";
    case 0x08: return "LEFT_DOWN";
    case 0x09: return "ROOT_MENU";
    case 0x0a: return "SETUP_MENU";
    case 0x0b: return "CONTENTS_MENU";
    case 0x0c: return "FAVORITE_MENU";
    case 0x0d: return "EXIT";
    case 0x2a: return "DOT";
    case 0x2b: return "ENTER";
    case 0x2c: return "CLEAR";
    case 0x30: return "CHANNEL_UP";
    case 0x31: return "CHANNEL_DOWN";
    case 0x32: return "PREVIOUS_CHANNEL";
    case 0x33: return "SOUND_SELECT";
    case 0x34: return "INPUT_SELECT";
    case 0x35: return "DISPLAY_INFORMATION";
    case 0x36: return "HELP";
    case 0x37: return "PAGE_UP";
    case 0x38: return "PAGE_DOWN";
    case 0x40: return "POWER";
    case 0x41: return "VOLUME_UP";
    case 0x42: return "VOLUME_DOWN";
    case 0x43: return "MUTE";
    case 0x44: return "PLAY";
    case 0x45: return "STOP";
    case 0x46: return "PAUSE";
    case 0x47: return "RECORD";
    case 0x48: return "REWIND";
    case 0x49: return "FAST_FORWARD";
    case 0x4a: return "EJECT";
    case 0x4b: return "FORWARD";
    case 0x4c: return "BACKWARD";
    case 0x50: return "ANGLE";
    case 0x51: return "SUBPICTURE";
    case 0x71: return "F1";
    case 0x72: return "F2";
    case 0x73: return "F3";
    case 0x74: return "F4";
    case 0x75: return "F5";
    case 0x7e: return "VENDOR_UNIQUE";
    default: return nullptr;
  }
}

// An AV/C frame on the control channel: PASS THROUGH, vendor-dependent AVRCP PDUs, unit info.
std::string avc(Out& o, const uint8_t* p, size_t m, bool rsp) {
  const uint8_t ctype = p[0] & 0x0f, opcode = p[2];
  OPEN("AV/C", avc_ctype_name(ctype), p, m);
  F("Type", sf("0x%x (%s)", ctype, avc_ctype_name(ctype)), p, 1);
  F("Subunit", sf("type 0x%02x%s, id %u", p[1] >> 3, (p[1] >> 3) == 0x09 ? " (panel)" : (p[1] >> 3) == 0x1f ? " (unit)" : "",
                  p[1] & 7), p + 1, 1);
  std::string s;
  if (opcode == 0x7c && m >= 5) {  // PASS THROUGH
    const uint8_t op = p[3] & 0x7f;
    const char* on = passthrough_name(op);
    F("Opcode", "PASS THROUGH", p + 2, 1);
    F("Operation", sf("0x%02x (%s) %s", op, on ? on : "?", p[3] & 0x80 ? "released" : "pressed"), p + 3, 1);
    F("Operand length", std::to_string(p[4]), p + 4, 1);
    s = sf("PASS THROUGH %s %s %s", on ? on : sf("0x%02x", op).c_str(), p[3] & 0x80 ? "released" : "pressed",
           avc_ctype_name(ctype));
    if (op == 0x7e && m >= 10) {  // vendor unique: company, then the vendor's operation
      const uint32_t c = (p[5] << 16) | (p[6] << 8) | p[7];
      const uint16_t vop = be16(p + 8);
      const char* vn = c == 0x001958 && vop == 0 ? "NEXT_GROUP" : c == 0x001958 && vop == 1 ? "PREV_GROUP" : nullptr;
      F("Company", sf("0x%06x%s", c, c == 0x001958 ? " (Bluetooth SIG)" : ""), p + 5, 3);
      F("Vendor operation", sf("0x%04x%s%s", vop, vn ? " " : "", vn ? vn : ""), p + 8, 2);
      s = sf("PASS THROUGH %s %s %s", vn ? vn : sf("vendor 0x%04x", vop).c_str(), p[3] & 0x80 ? "released" : "pressed",
             avc_ctype_name(ctype));
    }
  } else if (opcode == 0x00 && m >= 10) {  // VENDOR DEPENDENT: company, PDU, packet type, length
    const uint8_t pdu = p[6], pt = p[7] & 3;
    const char* pn = avrcp_pdu_name(pdu);
    static const char* pts[] = {"single", "start", "continue", "end"};
    F("Opcode", "VENDOR DEPENDENT", p + 2, 1);
    const uint32_t company = (p[3] << 16) | (p[4] << 8) | p[5];
    F("Company", sf("0x%06x%s", company, company == 0x001958 ? " (Bluetooth SIG)" : ""), p + 3, 3);
    F("PDU", sf("0x%02x (%s)", pdu, pn ? pn : "?"), p + 6, 1);
    F("Packet type", sf("%u (%s)", pt, pts[pt]), p + 7, 1);
    F("Parameter length", std::to_string(be16(p + 8)), p + 8, 2);
    s = std::string(pn ? pn : sf("PDU 0x%02x", pdu).c_str()) + " " + avc_ctype_name(ctype);
    const uint8_t* v = p + 10;
    const size_t vn = std::min<size_t>(m - 10, be16(p + 8));
    if (pt == 2 || pt == 3) {
      // The rest of a response too long for one AV/C frame: no header of its own to decode from.
      if (vn) F("Continuation", hexbytes(v, vn, 64), v, vn);
      s += sf(" (%s, %zu bytes)", pts[pt], vn);
    } else {
      o.mark(v);
      const std::string d = avrcp_vendor_params(o, pdu, rsp, ctype, v, vn);
      if (pt == 1) s += " (start)";
      // "RegisterNotification CHANGED VOLUME_CHANGED: 87 (69%)": the event reads as part of the name.
      if (!d.empty()) s += (pdu == 0x31 && ctype != 0xa ? " " : ": ") + d;
      rest(o, v, vn);
    }
  } else {
    F("Opcode", sf("0x%02x%s", opcode, opcode == 0x30 ? " (UNIT INFO)" : opcode == 0x31 ? " (SUBUNIT INFO)" : ""), p + 2, 1);
    s = sf("%s %s", opcode == 0x30 ? "UNIT INFO" : opcode == 0x31 ? "SUBUNIT INFO" : "AV/C", avc_ctype_name(ctype));
    if (m > 3) F("Operands", hexbytes(p + 3, m - 3), p + 3, m - 3);
  }
  CLOSE();
  return s;
}

std::string avctp(Out& o, const uint8_t* d, size_t n, bool browsing) {
  if (n < 1) return "AVCTP (truncated)";
  const uint8_t label = d[0] >> 4, ptype = (d[0] >> 2) & 3;
  const bool rsp = d[0] & 0x02;
  static const char* ptypes[] = {"single", "start", "continue", "end"};
  // A start packet has the number of packets before the PID; continue and end packets only the
  // first byte (AVCTP 6.1.2).
  const size_t hdr = ptype == 0 ? 3 : ptype == 1 ? 4 : 1;
  if (n < hdr) return "AVCTP (truncated)";
  OPEN("AVCTP", "", d, hdr);
  F("Header", sf("label %u, %s packet, %s%s", label, ptypes[ptype], rsp ? "response" : "command",
                 d[0] & 1 ? ", invalid PID" : ""), d, 1);
  if (ptype == 1) F("Number of packets", std::to_string(d[1]), d + 1, 1);
  if (ptype <= 1) {
    const uint16_t pid = be16(d + hdr - 2);
    F("PID", sf("0x%04x%s", pid, pid == 0x110e ? " (A/V Remote Control)" : pid == 0x110c ? " (A/V Remote Control Target)" : ""),
      d + hdr - 2, 2);
  }
  CLOSE();
  const uint8_t* p = d + hdr;
  const size_t m = n - hdr;
  if (ptype >= 2) {
    if (m) F("Data", hexbytes(p, m, 64), p, m);
    return sf("AVCTP %s packet (label %u), %zu bytes", ptypes[ptype], label, m);
  }
  if (browsing) {
    if (m < 3) return "AVRCP browsing (truncated)";
    return avrcp_browsing(o, p, m, rsp);
  }
  if (m < 3) return sf("AVCTP %s (label %u)", rsp ? "response" : "command", label);
  std::string s = avc(o, p, m, rsp);
  if (ptype == 1) s += sf(" [AVCTP start of %u]", d[1]);
  return s;
}

// ---------------------------------------------------------------------------------------------
// ACL → L2CAP → the channel's protocol

// L2CAP's FCS: CRC-16 (x^16 + x^15 + x^2 + 1), bits in reverse, starting at 0 (Core Vol 3 Part A
// 3.3.5), over the basic header, the control field and the payload.
uint16_t l2cap_fcs(const uint8_t* d, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc;
}

// An Enhanced Retransmission Mode frame: the AVRCP browsing channel always is one (AVRCP 1.6
// 6.1.2). frame/len: from the basic header to the end. Puts the control field (and the SDU length
// and FCS) in the tree; an unsegmented I-frame's SDU is returned in *sdu for the channel's
// protocol, anything else (S-frames, segments) gets its summary in *summary and returns false.
bool ertm(Out& o, const uint8_t* frame, size_t len, const uint8_t** sdu, size_t* sdu_len, std::string* summary) {
  const uint8_t* q = frame + 4;
  size_t m = len - 4;
  if (m < 2) {
    *summary = "ERTM (truncated)";
    return false;
  }
  // The FCS is there unless the channel switched it off (the CRC tells): checked, not assumed.
  bool fcs = m >= 4 && l2cap_fcs(frame, len - 2) == le16(frame + len - 2);
  if (fcs) m -= 2;
  const uint16_t c = le16(q);
  const unsigned reqseq = (c >> 8) & 0x3f, f = (c >> 7) & 1;
  std::string what;
  if (c & 1) {
    static const char* sv_short[] = {"RR", "REJ", "RNR", "SREJ"};
    static const char* sv_long[] = {"receiver ready", "reject", "receiver not ready", "selective reject"};
    const unsigned sv = (c >> 2) & 3;
    OPEN("ERTM", "S-frame", q, m);
    F("Control", sf("0x%04x: S-frame %s (%s), ReqSeq %u%s%s", c, sv_short[sv], sv_long[sv], reqseq, f ? ", F" : "",
                    (c >> 4) & 1 ? ", P" : ""), q, 2);
    if (fcs) F("FCS", sf("0x%04x (good)", le16(frame + len - 2)), frame + len - 2, 2);
    CLOSE();
    *summary = sf("ERTM %s ReqSeq %u", sv_short[sv], reqseq);
    return false;
  }
  static const char* sars[] = {"unsegmented", "start", "end", "continuation"};
  const unsigned txseq = (c >> 1) & 0x3f, sar = (c >> 14) & 3;
  OPEN("ERTM", sf("I-frame TxSeq %u", txseq), q, m);
  F("Control", sf("0x%04x: I-frame %s, TxSeq %u, ReqSeq %u%s", c, sars[sar], txseq, reqseq, f ? ", F" : ""), q, 2);
  size_t at = 2;
  if (sar == 1 && m >= 4) {
    F("SDU length", std::to_string(le16(q + 2)), q + 2, 2);
    at = 4;
  }
  if (fcs) F("FCS", sf("0x%04x (good)", le16(frame + len - 2)), frame + len - 2, 2);
  CLOSE();
  if (sar != 0) {
    F("Segment", hexbytes(q + at, m - at, 64), q + at, m - at);
    *summary = sf("ERTM I-frame %s, TxSeq %u, %zu bytes", sars[sar], txseq, m - at);
    return false;
  }
  *sdu = q + at;
  *sdu_len = m - at;
  return true;
}

// A channel whose protocol the index does not know. GOEP 2.0 profiles (PBAP, MAP, OPP, FTP) run
// OBEX straight over L2CAP in Enhanced Retransmission Mode: recognised by a good FCS, then by the
// OBEX packet inside. frame: the basic header; q/m: the payload (all of it: l2len == m).
std::string l2cap_other(Out& o, const uint8_t* frame, const uint8_t* q, size_t m, size_t l2len, uint16_t cid) {
  // The CRC costs a pass over the packet, and the text filter summarizes every packet: it is only
  // computed for short frames (S-frames, last segments) and for ones that would carry OBEX.
  const bool maybe_ertm = m == l2len && m >= 4 &&
                          (m <= 64 || is_obex(q + 2, m - 4) || (m >= 8 && is_obex(q + 4, m - 6)));
  if (maybe_ertm && l2cap_fcs(frame, 4 + m - 2) == le16(frame + 4 + m - 2)) {
    const uint8_t* sdu = nullptr;
    size_t sdu_len = 0;
    std::string s;
    if (!ertm(o, frame, 4 + m, &sdu, &sdu_len, &s)) {
      // The first segment of a long SDU (a large Get response): its OBEX header is here.
      const uint16_t c = le16(q);
      if (!(c & 1) && ((c >> 14) & 3) == 1 && m >= 8 && is_obex(q + 4, m - 6)) return obex(o, q + 4, m - 6) + " [SDU start]";
      return s;
    }
    if (is_obex(sdu, sdu_len)) return obex(o, sdu, sdu_len);
    F("Data", hexbytes(sdu, sdu_len, 64), sdu, sdu_len);
    return sf("CID 0x%04x, ERTM I-frame, %zu bytes", cid, sdu_len);
  }
  if (is_obex(q, m)) return obex(o, q, m);
  F("Data", hexbytes(q, m, 64), q, m);
  return sf("CID 0x%04x, %zu bytes", cid, m);
}

void acl(Out& o, const IndexEntry& e, const uint8_t* d, size_t len, const GattDb* gatt) {
  if (len < 4) {
    o.summary = "ACL (truncated)";
    return;
  }
  const uint16_t hf = le16(d);
  const uint8_t pb = (hf >> 12) & 3, bc = (hf >> 14) & 3;
  const uint16_t dlen = le16(d + 2);
  OPEN("HCI ACL", sf("handle %u", hf & 0x0fff), d, len);
  static const char* pbs[] = {"first non-flushable", "continuing", "first flushable", "complete"};
  F("Handle", std::to_string(hf & 0x0fff), d, 2);
  F("Packet boundary", sf("%u (%s)", pb, pbs[pb]), d + 1, 1);
  if (bc) F("Broadcast", std::to_string(bc), d + 1, 1);
  F("Data length", std::to_string(dlen), d + 2, 2);
  CLOSE();
  const uint8_t* p = d + 4;
  const size_t n = std::min<size_t>(len - 4, dlen);

  if (pb == 1) {
    OPEN("L2CAP continuation", "", p, n);
    if (e.cid) F("Channel (from its start)", cid_str(e.cid), nullptr, 0);
    if (e.psm) F("PSM", psm_str(e.psm), nullptr, 0);
    F("Data", hexbytes(p, n, 64), p, n);
    CLOSE();
    o.summary = sf("continuation, %zu bytes", n);
    return;
  }
  if (n < 4) {
    o.summary = "L2CAP (truncated)";
    return;
  }
  const uint16_t l2len = le16(p), cid = le16(p + 2);
  const uint8_t* q = p + 4;
  const size_t m = std::min<size_t>(n - 4, l2len);
  OPEN("L2CAP", sf("CID 0x%04x", cid), p, n);
  F("Length", std::to_string(l2len), p, 2);
  F("Channel ID", cid_str(cid), p + 2, 2);
  if (e.psm) F("PSM", psm_str(e.psm), nullptr, 0);
  if (m < l2len) F("Fragment", sf("first %zu of %u bytes", m, l2len), q, m);
  CLOSE();
  const std::string frag = m < l2len ? sf(" [%zu/%u]", m, l2len) : "";

  std::string s;
  switch (e.proto) {
    case kProtoL2cap:
      if (cid == kCidSignaling || cid == kCidLeSignaling) {
        OPEN(cid == kCidSignaling ? "L2CAP signalling" : "LE signalling", "", q, m);
        s = l2cap_sig(o, q, m, cid == kCidLeSignaling);
        CLOSE();
      } else {
        s = l2cap_other(o, p, q, m, l2len, cid);
      }
      break;
    case kProtoAtt: s = att(o, q, m, gatt, e.flags & kFlagTx); break;
    case kProtoSmp: s = smp(o, q, m); break;
    case kProtoSdp: s = sdp(o, q, m); break;
    case kProtoRfcomm: s = rfcomm(o, q, m); break;
    case kProtoAvdtp: s = avdtp(o, q, m); break;
    case kProtoRtp: s = rtp(o, q, m); break;
    case kProtoAvctp:
      if (e.psm == kPsmAvctpBrowsing && m == l2len) {
        const uint8_t* sdu = nullptr;
        size_t sdu_len = 0;
        if (ertm(o, p, 4 + m, &sdu, &sdu_len, &s)) s = avctp(o, sdu, sdu_len, true);
      } else {
        s = avctp(o, q, m, e.psm == kPsmAvctpBrowsing);
      }
      break;
    case kProtoBnep:
      F("BNEP", m ? sf("type 0x%02x", q[0] & 0x7f) : "", q, m);
      s = m ? sf("BNEP type 0x%02x, %zu bytes", q[0] & 0x7f, m) : "BNEP";
      break;
    case kProtoHid:
      F("HID", m ? sf("header 0x%02x", q[0]) : "", q, m);
      s = m ? sf("HID header 0x%02x, %zu bytes", q[0], m) : "HID";
      break;
    default:
      s = l2cap_other(o, p, q, m, l2len, cid);
      break;
  }
  o.summary = s + frag;
}

void sco_iso(Out& o, const uint8_t* d, size_t len, bool iso) {
  if (len < (iso ? 4u : 3u)) {
    o.summary = iso ? "ISO (truncated)" : "SCO (truncated)";
    return;
  }
  const uint16_t hf = le16(d);
  if (!iso) {
    static const char* st[] = {"correct", "possibly invalid", "no data", "partially lost"};
    OPEN("HCI SCO", sf("handle %u", hf & 0x0fff), d, len);
    F("Handle", std::to_string(hf & 0x0fff), d, 2);
    F("Packet status", st[(hf >> 12) & 3], d + 1, 1);
    F("Data length", std::to_string(d[2]), d + 2, 1);
    if (len > 3) F("Data", hexbytes(d + 3, len - 3, 32), d + 3, len - 3);
    CLOSE();
    o.summary = sf("SCO %zu bytes%s", len - 3, (hf >> 12) & 3 ? sf(" (%s)", st[(hf >> 12) & 3]).c_str() : "");
    return;
  }
  const uint8_t pb = (hf >> 12) & 3;
  const bool tsf = hf & 0x4000;
  const uint16_t dl = le16(d + 2) & 0x3fff;
  OPEN("HCI ISO", sf("handle %u", hf & 0x0fff), d, len);
  F("Handle", std::to_string(hf & 0x0fff), d, 2);
  static const char* const pbs[] = {"first fragment", "continuation", "complete SDU", "last fragment"};
  F("Packet boundary", sf("%u (%s)", pb, pbs[pb]), d + 1, 1);
  F("Timestamp present", tsf ? "yes" : "no", d + 1, 1);
  F("Data length", std::to_string(dl), d + 2, 2);
  std::string s = sf("ISO %zu bytes", len - 4);
  size_t at = 4;
  if (pb == 0 || pb == 2) {  // a first (or complete) fragment carries the SDU header
    if (tsf && at + 4 <= len) {
      F("Timestamp", sf("%u us", le32(d + at)), d + at, 4);
      at += 4;
    }
    if (at + 4 <= len) {
      // The packet status flag only means something from the controller (Core Vol 4 Part E 5.4.5).
      static const char* const st[] = {"valid", "possibly invalid", "lost", "?"};
      F("Sequence number", std::to_string(le16(d + at)), d + at, 2);
      F("SDU length", std::to_string(le16(d + at + 2) & 0x0fff), d + at + 2, 2);
      F("Packet status", st[le16(d + at + 2) >> 14], d + at + 3, 1);
      s = sf("ISO seq %u, SDU %u bytes", le16(d + at), le16(d + at + 2) & 0x0fff);
      if ((le16(d + at + 2) >> 14) == 2) s += " (lost)";
      at += 4;
    }
  }
  if (at < len) F("Data", hexbytes(d + at, len - at, 32), d + at, len - at);
  CLOSE();
  o.summary = s;
}

// ---------------------------------------------------------------------------------------------
// Monitor notices

void monitor_note(Out& o, const IndexEntry& e, const uint8_t* d, size_t len) {
  switch (e.mon) {
    case kMonNewIndex:
      if (len >= 16) {
        static const char* buses[] = {"virtual", "USB", "PC Card", "UART", "RS232", "PCI",
                                      "SDIO", "SPI", "I2C", "SMD", "VIRTIO", "IPC"};
        char name[9] = {};
        std::memcpy(name, d + 8, 8);
        const char* bus = d[1] < 12 ? buses[d[1]] : "?";
        OPEN("New Index", name, d, len);
        F("Type", d[0] == 0 ? "primary" : d[0] == 1 ? "AMP" : "?", d, 1);
        F("Bus", bus, d + 1, 1);
        F("Address", bdaddr_str(d + 2), d + 2, 6);
        F("Name", name, d + 8, 8);
        CLOSE();
        o.summary = sf("New Index %s %s (%s)", name, bdaddr_str(d + 2).c_str(), bus);
      } else {
        o.summary = "New Index";
      }
      return;
    case kMonDelIndex: o.summary = "Delete Index"; return;
    case kMonOpenIndex: o.summary = "Open Index"; return;
    case kMonCloseIndex: o.summary = "Close Index"; return;
    case kMonIndexInfo:
      if (len >= 8) {
        F("Address", bdaddr_str(d), d, 6);
        F("Manufacturer", std::to_string(le16(d + 6)), d + 6, 2);
        o.summary = sf("Index Info %s manufacturer %u", bdaddr_str(d).c_str(), le16(d + 6));
      } else {
        o.summary = "Index Info";
      }
      return;
    case kMonSystemNote: {
      const std::string t = printable(d, len, 200);
      F("Note", t, d, len);
      o.summary = t;
      return;
    }
    case kMonUserLogging: {
      // priority, ident length, ident (NUL-terminated), message
      if (len < 2 || 2u + d[1] > len) {
        o.summary = "User Logging (truncated)";
        return;
      }
      const std::string ident = printable(d + 2, d[1], 40);
      const std::string msg = printable(d + 2 + d[1], len - 2 - d[1], 300);
      F("Priority", std::to_string(d[0]), d, 1);
      F("Ident", ident, d + 2, d[1]);
      F("Message", msg, d + 2 + d[1], len - 2 - d[1]);
      o.summary = ident + ": " + msg;
      return;
    }
    case kMonCtrlOpen:
      if (len >= 14) {
        const std::string nm = printable(d + 14, len - 14, 32);
        F("Cookie", sf("0x%08x", le32(d)), d, 4);
        F("Format", std::to_string(le16(d + 4)), d + 4, 2);
        F("Version", sf("%u.%u", d[6], le16(d + 7)), d + 6, 3);
        F("Flags", sf("0x%08x", le32(d + 9)), d + 9, 4);
        if (len > 14) F("Name", nm, d + 14, len - 14);
        o.summary = sf("Control Open 0x%08x %s", le32(d), nm.c_str());
      } else {
        o.summary = "Control Open";
      }
      return;
    case kMonCtrlClose:
      o.summary = len >= 4 ? sf("Control Close 0x%08x", le32(d)) : "Control Close";
      return;
    case kMonCtrlCommand:
    case kMonCtrlEvent:
      if (len >= 6) dissect_mgmt(o, e.mon == kMonCtrlCommand, d, len);
      else o.summary = "MGMT";
      return;
    case kMonVendorDiag:
      F("Data", hexbytes(d, len, 64), d, len);
      o.summary = sf("Vendor diagnostic, %zu bytes", len);
      return;
    default:
      if (len) F("Data", hexbytes(d, len, 64), d, len);
      o.summary = sf("Monitor opcode %u", e.mon);
      return;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------

bool hci_cmd_has_handle(uint16_t op) {
  switch (op) {
    case 0x0406: case 0x040f: case 0x0411: case 0x0413: case 0x0415: case 0x041b: case 0x041c:
    case 0x041d: case 0x041f: case 0x0420: case 0x0428: case 0x043d:
    case 0x0801: case 0x0803: case 0x0804: case 0x0807: case 0x0809: case 0x080c: case 0x080d:
    case 0x0810: case 0x0811:
    case 0x0c08: case 0x0c27: case 0x0c28: case 0x0c2d: case 0x0c36: case 0x0c37: case 0x0c5f:
    case 0x0c7b: case 0x0c7c:
    case 0x1401: case 0x1402: case 0x1403: case 0x1405: case 0x1406: case 0x1408:
    case 0x2013: case 0x2015: case 0x2016: case 0x2019: case 0x201a: case 0x201b: case 0x2020:
    case 0x2021: case 0x2022: case 0x2030: case 0x2032: case 0x2061: case 0x2066: case 0x206e:
    case 0x206f: case 0x2075:
      return true;
    default:
      return false;
  }
}

bool l2cap_payload_is_error(uint8_t proto, uint16_t cid, const uint8_t* d, size_t n) {
  if (n < 1) return false;
  switch (proto) {
    case kProtoL2cap:
      if (cid != kCidSignaling && cid != kCidLeSignaling) return false;
      while (n >= 4) {
        const uint8_t code = d[0];
        const size_t clen = le16(d + 2);
        const uint8_t* p = d + 4;
        const size_t m = std::min(clen, n - 4);
        if (code == 0x01) return true;
        if ((code == 0x03 || code == 0x0d) && m >= 6 && le16(p + 4) > 1) return true;
        if (code == 0x05 && m >= 6 && le16(p + 4) != 0 && le16(p + 4) != 4) return true;
        if (code == 0x15 && m >= 10 && le16(p + 8) != 0) return true;
        if (code == 0x18 && m >= 8 && le16(p + 6) != 0) return true;
        if (code == 0x13 && m >= 2 && le16(p) != 0) return true;
        if (clen > n - 4) break;
        d += 4 + clen;
        n -= 4 + clen;
      }
      return false;
    case kProtoAtt: return d[0] == 0x01;
    case kProtoSmp: return d[0] == 0x05;
    case kProtoSdp: return d[0] == 0x01;
    case kProtoAvdtp: {
      const uint8_t ptype = (d[0] >> 2) & 3, mtype = d[0] & 3;
      return (ptype == 0 || ptype == 1) && (mtype == 1 || mtype == 3);
    }
    case kProtoRfcomm: return n >= 2 && (d[1] & ~0x10) == 0x0f;  // DM
    case kProtoAvctp:
      // AV/C REJECTED or NOT_IMPLEMENTED on the control channel.
      return n >= 4 && ((d[0] >> 2) & 3) == 0 && (d[0] & 0x02) &&
             ((d[3] & 0x0f) == 0x0a || (d[3] & 0x0f) == 0x08);
    default:
      return false;
  }
}

namespace {

void put_attr(std::vector<std::pair<uint16_t, uint16_t>>* v, uint16_t h, uint16_t uuid) {
  auto it = std::lower_bound(v->begin(), v->end(), std::make_pair(h, uint16_t{0}),
                             [](const std::pair<uint16_t, uint16_t>& a, const std::pair<uint16_t, uint16_t>& b) {
                               return a.first < b.first;
                             });
  if (it != v->end() && it->first == h) {
    // A Find Information of a 128-bit UUID says 0: keep what the declaration said.
    if (uuid) it->second = uuid;
  } else {
    v->insert(it, {h, uuid});
  }
}

}  // namespace

void GattDb::clear() {
  side[0] = Side();
  side[1] = Side();
}

void GattDb::learn(const uint8_t* p, size_t n, bool tx) {
  if (n < 1) return;
  const uint8_t op = p[0];
  Side& s = side[((op & 1) != 0) == tx ? 1 : 0];
  switch (op) {
    case 0x08:  // Read By Type Request: start, end, type (a 128-bit one is not followed)
      if (n >= 7) s.type_req = n == 7 ? le16(p + 5) : 0;
      break;
    case 0x0a:  // Read Request, Read Blob Request
    case 0x0c:
      if (n >= 3) s.read_handle = le16(p + 1);
      break;
    case 0x09: {  // Read By Type Response
      if (n < 2) break;
      const size_t el = p[1];
      if (el < 3) break;
      const bool decl = s.type_req == 0x2803 || (!s.type_req && (el == 7 || el == 21));
      for (size_t i = 2; i + el <= n; i += el) {
        const uint8_t* e = p + i;
        if (decl && (el == 7 || el == 21)) {
          const uint16_t vh = le16(e + 3);
          put_attr(&s.attrs, le16(e), 0x2803);
          put_attr(&s.attrs, vh, el == 7 ? le16(e + 5) : 0);
          auto it = std::lower_bound(s.values.begin(), s.values.end(), vh);
          if (it == s.values.end() || *it != vh) s.values.insert(it, vh);
        } else if (s.type_req && s.type_req != 0x2802) {
          put_attr(&s.attrs, le16(e), s.type_req);
        }
      }
      break;
    }
    case 0x05: {  // Find Information Response
      if (n < 2) break;
      const size_t ul = p[1] == 1 ? 2 : 16;
      for (size_t i = 2; i + 2 + ul <= n; i += 2 + ul) put_attr(&s.attrs, le16(p + i), ul == 2 ? le16(p + i + 2) : 0);
      break;
    }
    default:
      break;
  }
}

void GattDb::learn_acl(const uint8_t* d, size_t len, bool tx) {
  if (len < 9 || ((le16(d) >> 12) & 3) == 1) return;  // a continuation has no ATT header
  learn(d + 8, std::min<size_t>(len - 8, le16(d + 4)), tx);
}

uint16_t GattDb::uuid(bool local_server, uint16_t handle) const {
  const auto& v = side[local_server ? 1 : 0].attrs;
  auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(handle, uint16_t{0}),
                             [](const std::pair<uint16_t, uint16_t>& a, const std::pair<uint16_t, uint16_t>& b) {
                               return a.first < b.first;
                             });
  return it != v.end() && it->first == handle ? it->second : 0;
}

uint16_t GattDb::owner(bool local_server, uint16_t handle) const {
  const auto& v = side[local_server ? 1 : 0].values;
  auto it = std::lower_bound(v.begin(), v.end(), handle);
  return it == v.begin() ? 0 : *(it - 1);
}

void dissect(const IndexEntry& e, const uint8_t* d, size_t len, bool detail, Dissection* out, const GattDb* gatt,
             uint16_t manufacturer) {
  Out o(d, detail);
  o.manufacturer = manufacturer;
  switch (e.mon) {
    case kMonCommand: hci_command(o, d, len); break;
    case kMonEvent: hci_event(o, d, len); break;
    case kMonAclTx:
    case kMonAclRx: acl(o, e, d, len, gatt); break;
    case kMonScoTx:
    case kMonScoRx: sco_iso(o, d, len, false); break;
    case kMonIsoTx:
    case kMonIsoRx: sco_iso(o, d, len, true); break;
    default: monitor_note(o, e, d, len); break;
  }
  out->summary = std::move(o.summary);
  out->fields = detail ? std::move(o.root) : json::array();
}

}  // namespace btb::hci
