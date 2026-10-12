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

  const bool detail;
  std::string summary;
  json root = json::array();

 private:
  json node(const std::string& name, const std::string& value, const uint8_t* p, size_t n) const {
    json j{{"name", name}};
    if (!value.empty()) j["value"] = value;
    if (p && n) {
      j["off"] = p - base_;
      j["len"] = n;
    }
    return j;
  }
  const uint8_t* base_;
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
    case 0x204b: return "LE Read Transmit Power";
    case 0x204e: return "LE Set Privacy Mode";
    case 0x2060: return "LE Read Buffer Size v2";
    case 0x2061: return "LE Read ISO TX Sync";
    case 0x2062: return "LE Set CIG Parameters";
    case 0x2063: return "LE Set CIG Parameters Test";
    case 0x2064: return "LE Create CIS";
    case 0x2065: return "LE Remove CIG";
    case 0x2066: return "LE Accept CIS Request";
    case 0x2067: return "LE Reject CIS Request";
    case 0x2068: return "LE Create BIG";
    case 0x206a: return "LE Terminate BIG";
    case 0x206b: return "LE BIG Create Sync";
    case 0x206c: return "LE BIG Terminate Sync";
    case 0x206e: return "LE Setup ISO Data Path";
    case 0x206f: return "LE Remove ISO Data Path";
    case 0x2074: return "LE Set Host Feature";
    case 0x2075: return "LE Read ISO Link Quality";
    default: return nullptr;
  }
}

std::string cmd_label(uint16_t op) {
  if (const char* n = cmd_name(op)) return n;
  if ((op >> 10) == 0x3f) return sf("Vendor 0x%04x", op);
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
    case 0x22: return "LE BIGInfo Advertising Report";
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
      case 0x001d: case 0x001e: case 0x001f: case 0x0021: case 0x0026: case 0x0027: case 0x0031:
      case 0x0032: case 0x0033: case 0x0034: case 0x0044: case 0x0050: case 0x0055: case 0x0056:
        return true;
      default: return false;
    }
  }
  switch (code) {
    case 0x000b: case 0x000c: case 0x000d: case 0x000e: case 0x000f: case 0x0010: case 0x0011:
    case 0x0012: case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x001a: case 0x001b:
    case 0x001c: case 0x002f: case 0x0030:
      return true;
    default: return false;
  }
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
    // The Set commands that change a setting answer with the settings in force after it.
    if (code == 0x0001 && !p[-1] && n == 4 && ((op >= 0x0005 && op <= 0x000d) || op == 0x0029 || op == 0x002a ||
                                               op == 0x002d || op == 0x002e || op == 0x002f)) {
      F("Current settings", mgmt_settings(le32(p)), p, 4);
      o.summary += " → " + mgmt_settings(le32(p));
      return;
    }
    if (n) F("Return parameters", hexbytes(p, n, 64), p, n);
    return;
  }
  const std::string name = mgmt_label(cmd ? mgmt_op_name(code) : mgmt_event_name(code), code);
  F(cmd ? "Opcode" : "Event", sf("%s (0x%04x)", name.c_str(), code), d + 4, 2);
  o.summary = std::string(cmd ? "MGMT " : "MGMT Event: ") + name;
  if (mgmt_has_addr(cmd, code) && n >= 7) {
    F("Address", bdaddr_str(p), p, 6);
    F("Address type", mgmt_addr_type(p[6]), p + 6, 1);
    o.summary += sf(" %s (%s)", bdaddr_str(p).c_str(), mgmt_addr_type(p[6]));
    p += 7;
    n -= 7;
    if (!cmd && code == 0x000c && n >= 1) {
      static const char* const kWhy[] = {"unknown", "timeout", "local host", "remote", "auth failure", "local host suspend"};
      const char* why = p[0] < 6 ? kWhy[p[0]] : "?";
      F("Reason", sf("0x%02x (%s)", p[0], why), p, 1);
      o.summary += sf(", reason %s", why);
      p++;
      n--;
    } else if (!cmd && (code == 0x000d || code == 0x0011) && n >= 1) {
      const char* st = mgmt_status_name(p[0]);
      F("Status", sf("0x%02x (%s)", p[0], st ? st : "unknown"), p, 1);
      o.summary += sf(", %s", st ? st : "failed");
      p++;
      n--;
    }
  } else if (cmd && ((code >= 0x0005 && code <= 0x000d) || code == 0x0029 || code == 0x002a || code == 0x002d) && n >= 1) {
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
  if (n) F("Data", hexbytes(p, n, 64), p, n);
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
    case 0x10: return "Device ID";
    case 0x14: return "16-bit solicitation UUIDs";
    case 0x16: return "Service data (16-bit UUID)";
    case 0x19: return "Appearance";
    case 0x1b: return "LE device address";
    case 0x20: return "Service data (32-bit UUID)";
    case 0x21: return "Service data (128-bit UUID)";
    case 0x24: return "URI";
    case 0x30: return "Broadcast name";
    case 0xff: return "Manufacturer data";
    default: return nullptr;
  }
}

// Returns the local name, if the data carries one; in detail mode lists every structure.
std::string ad_data(Out& o, const uint8_t* p, size_t n) {
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
      const char* tn = ad_type_name(t);
      std::string val;
      switch (t) {
        case 0x02:
        case 0x03:
        case 0x14:
          for (size_t i = 0; i + 1 < vn; i += 2) val += (val.empty() ? "" : ", ") + uuid_str(v + i, 2);
          break;
        case 0x06:
        case 0x07:
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
        case 0x01:
          if (vn >= 1) val = sf("0x%02x", v[0]);
          break;
        case 0x19:
          if (vn >= 2) val = sf("0x%04x", le16(v));
          break;
        case 0xff:
          if (vn >= 2) val = sf("company 0x%04x, ", le16(v)) + hexbytes(v + 2, vn - 2);
          break;
        case 0x16:
          if (vn >= 2) val = uuid_str(v, 2) + ": " + hexbytes(v + 2, vn - 2);
          break;
        default:
          val = hexbytes(v, vn);
      }
      o.add(tn ? tn : sf("AD type 0x%02x", t), val, p, l + 1);
    }
    p += l + 1;
    n -= l + 1;
  }
  return name;
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

void hci_command(Out& o, const uint8_t* d, size_t len) {
  if (len < 3) {
    o.summary = "Command (truncated)";
    return;
  }
  const uint16_t op = le16(d);
  const uint8_t* p = d + 3;
  const size_t n = std::min<size_t>(d[2], len - 3);
  const std::string name = cmd_label(op);

  OPEN("HCI Command", name, d, len);
  F("Opcode", sf("0x%04x (OGF 0x%02x, OCF 0x%03x) %s", op, op >> 10, op & 0x3ff, name.c_str()), d, 2);
  F("Parameter length", std::to_string(d[2]), d + 2, 1);

  std::string s = name;
  auto handle_field = [&](size_t at) {
    if (n >= at + 2) {
      const uint16_t h = le16(p + at) & 0x0fff;
      F("Handle", std::to_string(h), p + at, 2);
      return sf(" handle %u", h);
    }
    return std::string();
  };

  switch (op) {
    case 0x0405:  // Create Connection
    case 0x0419:  // Remote Name Request
    case 0x0409:  // Accept Connection Request
    case 0x040a:  // Reject Connection Request
    case 0x040b:
    case 0x040c:
    case 0x040d:
    case 0x040e:
    case 0x042b:
    case 0x042c:
    case 0x042d:
    case 0x042e:
    case 0x042f:
    case 0x0434:
    case 0x080b:  // Switch Role
    case 0x0408:
    case 0x041a:
      if (n >= 6) {
        F("Address", bdaddr_str(p), p, 6);
        s += " " + bdaddr_str(p);
      }
      if (op == 0x040a && n >= 7) {
        F("Reason", status_str(p[6]), p + 6, 1);
      } else if ((op == 0x0409 || op == 0x080b) && n >= 7) {
        F("Role", p[6] ? "peripheral" : "central", p + 6, 1);
        s += p[6] ? " (peripheral)" : " (central)";
      } else if (op == 0x042b && n >= 9) {
        F("IO capability", std::to_string(p[6]), p + 6, 1);
        F("OOB data", std::to_string(p[7]), p + 7, 1);
        F("Authentication", sf("0x%02x", p[8]), p + 8, 1);
      } else if (op == 0x042e && n >= 10) {
        F("Passkey", sf("%06u", le32(p + 6)), p + 6, 4);
      } else if (op == 0x0405 && n >= 13) {
        F("Packet type", sf("0x%04x", le16(p + 6)), p + 6, 2);
        F("Allow role switch", p[12] ? "yes" : "no", p + 12, 1);
      }
      break;
    case 0x0406:  // Disconnect
      s += handle_field(0);
      if (n >= 3) {
        F("Reason", status_str(p[2]), p + 2, 1);
        s += sf(" reason 0x%02x", p[2]);
      }
      break;
    case 0x0413:  // Set Connection Encryption
      s += handle_field(0);
      if (n >= 3) {
        F("Encryption", p[2] ? "on" : "off", p + 2, 1);
        s += p[2] ? " on" : " off";
      }
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
      if (n >= 4) F("Policy", sf("0x%04x", le16(p + 2)), p + 2, 2);
      break;
    case 0x0c13:  // Write Local Name
      if (n) {
        const std::string nm = printable(p, n, 64);
        F("Name", "\"" + nm + "\"", p, n);
        s += " \"" + nm + "\"";
      }
      break;
    case 0x0c1a:  // Write Scan Enable
      if (n >= 1) {
        static const char* modes[] = {"off", "inquiry scan", "page scan", "inquiry + page scan"};
        const char* m = p[0] < 4 ? modes[p[0]] : "?";
        F("Scan enable", m, p, 1);
        s += std::string(" ") + m;
      }
      break;
    case 0x0c24:  // Write Class of Device
      if (n >= 3) {
        F("Class", cod_str(le24(p)), p, 3);
        s += " " + cod_str(le24(p));
      }
      break;
    case 0x0c52:  // Write Extended Inquiry Response
      if (n >= 1) {
        F("FEC required", p[0] ? "yes" : "no", p, 1);
        OPEN("EIR", "", p + 1, n - 1);
        const std::string name = ad_data(o, p + 1, n - 1);
        CLOSE();
        if (!name.empty()) s += " \"" + name + "\"";
      }
      break;
    case 0x0c2f:  // Write Synchronous Flow Control Enable
    case 0x0c56:  // Write Simple Pairing Mode
    case 0x0c7a:
    case 0x200a:  // LE Set Advertising Enable
      if (n >= 1) {
        F("Enable", p[0] ? "yes" : "no", p, 1);
        s += p[0] ? " on" : " off";
      }
      break;
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
        F("Filter duplicates", std::to_string(p[1]), p + 1, 1);
        F("Duration", sf("%u ms", le16(p + 2) * 10), p + 2, 2);
        F("Period", sf("%.2f s", le16(p + 4) * 1.28), p + 4, 2);
        s += p[0] ? " on" : " off";
      }
      break;
    case 0x2039:  // LE Set Extended Advertising Enable
      if (n >= 2) {
        F("Enable", p[0] ? "yes" : "no", p, 1);
        F("Number of sets", std::to_string(p[1]), p + 1, 1);
        s += p[0] ? " on" : " off";
        for (size_t i = 0; i < p[1] && 2 + 4 * (i + 1) <= n; ++i) {
          const uint8_t* q = p + 2 + 4 * i;
          F(sf("Set %zu", i), sf("handle %u, duration %u ms, max events %u", q[0], le16(q + 1) * 10, q[3]), q, 4);
        }
      }
      break;
    case 0x200b:  // LE Set Scan Parameters
      if (n >= 7) {
        F("Type", p[0] ? "active" : "passive", p, 1);
        F("Interval", sf("%.2f ms", le16(p + 1) * 0.625), p + 1, 2);
        F("Window", sf("%.2f ms", le16(p + 3) * 0.625), p + 3, 2);
        F("Own address type", addr_type_name(p[5]), p + 5, 1);
        F("Filter policy", std::to_string(p[6]), p + 6, 1);
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
      if (n >= 7) {
        F("Address type", addr_type_name(p[0]), p, 1);
        F("Address", bdaddr_str(p + 1), p + 1, 6);
        s += " " + bdaddr_str(p + 1);
      }
      break;
    case 0x2027:  // LE Add Device To Resolving List
    case 0x2028:
      if (n >= 7) {
        F("Address type", addr_type_name(p[0]), p, 1);
        F("Address", bdaddr_str(p + 1), p + 1, 6);
        s += " " + bdaddr_str(p + 1);
      }
      break;
    case 0x200d:  // LE Create Connection
      if (n >= 25) {
        F("Scan interval", sf("%.2f ms", le16(p) * 0.625), p, 2);
        F("Scan window", sf("%.2f ms", le16(p + 2) * 0.625), p + 2, 2);
        F("Filter policy", std::to_string(p[4]), p + 4, 1);
        F("Peer address type", addr_type_name(p[5]), p + 5, 1);
        F("Peer address", bdaddr_str(p + 6), p + 6, 6);
        F("Own address type", addr_type_name(p[12]), p + 12, 1);
        F("Interval", sf("%.2f-%.2f ms", le16(p + 13) * 1.25, le16(p + 15) * 1.25), p + 13, 4);
        F("Latency", std::to_string(le16(p + 17)), p + 17, 2);
        F("Supervision timeout", sf("%u ms", le16(p + 19) * 10), p + 19, 2);
        s += p[4] ? " (accept list)" : " " + bdaddr_str(p + 6);
      }
      break;
    case 0x2043:  // LE Extended Create Connection
      if (n >= 10) {
        F("Filter policy", std::to_string(p[0]), p, 1);
        F("Own address type", addr_type_name(p[1]), p + 1, 1);
        F("Peer address type", addr_type_name(p[2]), p + 2, 1);
        F("Peer address", bdaddr_str(p + 3), p + 3, 6);
        F("PHYs", sf("0x%02x", p[9]), p + 9, 1);
        s += p[0] ? " (accept list)" : " " + bdaddr_str(p + 3);
      }
      break;
    case 0x2013:  // LE Connection Update
      s += handle_field(0);
      if (n >= 10) {
        F("Interval", sf("%.2f-%.2f ms", le16(p + 2) * 1.25, le16(p + 4) * 1.25), p + 2, 4);
        F("Latency", std::to_string(le16(p + 6)), p + 6, 2);
        F("Supervision timeout", sf("%u ms", le16(p + 8) * 10), p + 8, 2);
        s += sf(" interval %.2f-%.2f ms", le16(p + 2) * 1.25, le16(p + 4) * 1.25);
      }
      break;
    case 0x2019:  // LE Enable Encryption
      s += handle_field(0);
      if (n >= 28) {
        F("Random", hexbytes(p + 2, 8), p + 2, 8);
        F("EDIV", sf("0x%04x", le16(p + 10)), p + 10, 2);
        F("LTK", hexbytes(p + 12, 16), p + 12, 16);
      }
      break;
    case 0x2022:  // LE Set Data Length
      s += handle_field(0);
      if (n >= 6) {
        F("TX octets", std::to_string(le16(p + 2)), p + 2, 2);
        F("TX time", sf("%u us", le16(p + 4)), p + 4, 2);
        s += sf(" %u octets", le16(p + 2));
      }
      break;
    default:
      if (hci_cmd_has_handle(op)) s += handle_field(0);
      break;
  }
  if (o.detail && n) F("Parameters", hexbytes(p, n, 64), p, n);
  CLOSE();
  o.summary = s;
}

// ---------------------------------------------------------------------------------------------
// HCI events

// The return parameters of a Command Complete for the commands people read the answer of.
std::string cmd_return(Out& o, uint16_t op, const uint8_t* r, size_t n) {
  if (n < 1) return {};
  F("Status", status_str(r[0]), r, 1);
  std::string s = r[0] ? sf(" status 0x%02x (%s)", r[0], error_name(r[0])) : "";
  if (r[0] != 0) return s;
  switch (op) {
    case 0x1001:
      if (n >= 9) {
        F("HCI version", sf("%u (revision 0x%04x)", r[1], le16(r + 2)), r + 1, 3);
        F("LMP version", sf("%u (subversion 0x%04x)", r[4], le16(r + 7)), r + 4, 1);
        F("Manufacturer", sf("%u", le16(r + 5)), r + 5, 2);
        s += sf(" HCI %u, manufacturer %u", r[1], le16(r + 5));
      }
      break;
    case 0x1009:
      if (n >= 7) {
        F("Address", bdaddr_str(r + 1), r + 1, 6);
        s += " " + bdaddr_str(r + 1);
      }
      break;
    case 0x0c14:
      if (n >= 2) {
        const std::string nm = printable(r + 1, n - 1, 64);
        F("Name", "\"" + nm + "\"", r + 1, n - 1);
        s += " \"" + nm + "\"";
      }
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
    case 0x1405:  // Read RSSI
      if (n >= 4) {
        F("Handle", std::to_string(le16(r + 1) & 0x0fff), r + 1, 2);
        F("RSSI", sf("%d dB", static_cast<int8_t>(r[3])), r + 3, 1);
        s += sf(" handle %u RSSI %d", le16(r + 1) & 0x0fff, static_cast<int8_t>(r[3]));
      }
      break;
    case 0x1408:  // Read Encryption Key Size
      if (n >= 4) {
        F("Handle", std::to_string(le16(r + 1) & 0x0fff), r + 1, 2);
        F("Key size", std::to_string(r[3]), r + 3, 1);
        s += sf(" handle %u key size %u", le16(r + 1) & 0x0fff, r[3]);
      }
      break;
    default:
      if (hci_cmd_has_handle(op) && n >= 3) {
        F("Handle", std::to_string(le16(r + 1) & 0x0fff), r + 1, 2);
        s += sf(" handle %u", le16(r + 1) & 0x0fff);
      }
      break;
  }
  if (o.detail && n > 1) F("Return parameters", hexbytes(r + 1, n - 1, 64), r + 1, n - 1);
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

std::string le_meta(Out& o, const uint8_t* p, size_t n) {
  if (n < 1) return "LE Meta Event (truncated)";
  const uint8_t sub = p[0];
  const char* nm = le_sub_name(sub);
  std::string s = nm ? nm : sf("LE Meta 0x%02x", sub);
  F("Subevent", sf("0x%02x (%s)", sub, nm ? nm : "unknown"), p, 1);
  const uint8_t* q = p + 1;
  const size_t m = n - 1;

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
        if (m >= at + 6) {
          F("Interval", sf("%.2f ms", le16(q + at) * 1.25), q + at, 2);
          F("Latency", std::to_string(le16(q + at + 2)), q + at + 2, 2);
          F("Supervision timeout", sf("%u ms", le16(q + at + 4) * 10), q + at + 4, 2);
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
        const uint8_t* r = q + off;
        const size_t dl = r[8];
        if (off + 9 + dl + 1 > m) break;
        const int8_t rssi = static_cast<int8_t>(r[9 + dl]);
        OPEN(sf("Report %zu", i), bdaddr_str(r + 2), r, 10 + dl);
        F("Event type", r[0] < 5 ? types[r[0]] : "?", r, 1);
        F("Address type", addr_type_name(r[1]), r + 1, 1);
        F("Address", bdaddr_str(r + 2), r + 2, 6);
        const std::string name = ad_data(o, r + 9, dl);
        F("RSSI", sf("%d dBm", rssi), r + 9 + dl, 1);
        CLOSE();
        if (i == 0) {
          first = sf(" %s %s (%s) %d dBm", r[0] < 5 ? types[r[0]] : "?", bdaddr_str(r + 2).c_str(),
                     addr_type_name(r[1]), rssi);
          if (!name.empty()) first += " \"" + name + "\"";
        }
        off += 10 + dl;
      }
      s += first;
      if (q[0] > 1) s += sf(" (+%u more)", q[0] - 1);
      break;
    }
    case 0x0d: {  // Extended Advertising Report
      if (m < 1) break;
      F("Reports", std::to_string(q[0]), q, 1);
      size_t off = 1;
      std::string first;
      for (size_t i = 0; i < q[0] && off + 24 <= m; ++i) {
        const uint8_t* r = q + off;
        const size_t dl = r[23];
        if (off + 24 + dl > m) break;
        const uint16_t et = le16(r);
        const int8_t rssi = static_cast<int8_t>(r[13]);
        OPEN(sf("Report %zu", i), bdaddr_str(r + 3), r, 24 + dl);
        F("Event type", sf("0x%04x%s%s%s%s", et, et & 0x01 ? " connectable" : "",
                           et & 0x02 ? " scannable" : "", et & 0x08 ? " scan-response" : "",
                           et & 0x10 ? " legacy" : ""), r, 2);
        F("Address type", addr_type_name(r[2]), r + 2, 1);
        F("Address", bdaddr_str(r + 3), r + 3, 6);
        F("PHY", sf("primary %u, secondary %u", r[9], r[10]), r + 9, 2);
        F("SID", std::to_string(r[11]), r + 11, 1);
        F("TX power", sf("%d dBm", static_cast<int8_t>(r[12])), r + 12, 1);
        F("RSSI", sf("%d dBm", rssi), r + 13, 1);
        const std::string name = ad_data(o, r + 24, dl);
        CLOSE();
        if (i == 0) {
          first = sf(" %s (%s) %d dBm", bdaddr_str(r + 3).c_str(), addr_type_name(r[2]), rssi);
          if (!name.empty()) first += " \"" + name + "\"";
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
        F("Features", hexbytes(q + 3, 8), q + 3, 8);
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
    case 0x12:  // Advertising Set Terminated
      if (m >= 5) {
        F("Status", status_str(q[0]), q, 1);
        F("Advertising handle", std::to_string(q[1]), q + 1, 1);
        F("Connection handle", std::to_string(le16(q + 2) & 0x0fff), q + 2, 2);
        F("Completed events", std::to_string(q[4]), q + 4, 1);
        s += sf(" set %u", q[1]) + (q[0] ? " " + status_str(q[0]) : sf(" handle %u", le16(q + 2) & 0x0fff));
      }
      break;
    case 0x14:  // Channel Selection Algorithm
      if (m >= 3) {
        F("Handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("Algorithm", sf("#%u", q[2] + 1), q + 2, 1);
        s += sf(" handle %u #%u", le16(q) & 0x0fff, q[2] + 1);
      }
      break;
    case 0x19:
    case 0x2a:
      if (m >= 3) {
        F("Status", status_str(q[0]), q, 1);
        F("CIS handle", std::to_string(le16(q + 1) & 0x0fff), q + 1, 2);
        s += sf(" handle %u", le16(q + 1) & 0x0fff) + (q[0] ? " " + status_str(q[0]) : "");
      }
      break;
    case 0x1a:
      if (m >= 6) {
        F("ACL handle", std::to_string(le16(q) & 0x0fff), q, 2);
        F("CIS handle", std::to_string(le16(q + 2) & 0x0fff), q + 2, 2);
        F("CIG", std::to_string(q[4]), q + 4, 1);
        F("CIS", std::to_string(q[5]), q + 5, 1);
        s += sf(" ACL %u CIS %u", le16(q) & 0x0fff, le16(q + 2) & 0x0fff);
      }
      break;
    default:
      if (m >= 1 && (sub == 0x1b || sub == 0x1d || sub == 0x08 || sub == 0x09 || sub == 0x0e)) {
        F("Status", status_str(q[0]), q, 1);
        if (q[0]) s += " " + status_str(q[0]);
      }
      break;
  }
  if (o.detail && m) F("Parameters", hexbytes(q, m, 64), q, m);
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
      F("Command opcode", sf("0x%04x (%s)", op, cmd_label(op).c_str()), p + 1, 2);
      s = cmd_label(op) + " complete" + cmd_return(o, op, p + 3, n - 3);
      break;
    }
    case 0x0f: {  // Command Status
      if (n < 4) break;
      const uint16_t op = le16(p + 2);
      F("Status", status_str(p[0]), p, 1);
      F("Num HCI command packets", std::to_string(p[1]), p + 1, 1);
      F("Command opcode", sf("0x%04x (%s)", op, cmd_label(op).c_str()), p + 2, 2);
      s = cmd_label(op) + (p[0] ? " status " + status_str(p[0]) : " pending");
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
        F("LMP version", sf("%u (subversion 0x%04x)", p[3], le16(p + 6)), p + 3, 1);
        F("Manufacturer", std::to_string(le16(p + 4)), p + 4, 2);
        if (!p[0]) s += sf(" LMP %u manufacturer %u", p[3], le16(p + 4));
      }
      break;
    case 0x0b:  // Read Remote Supported Features Complete
      status_handle();
      if (n >= 11) F("Features", hexbytes(p + 3, 8), p + 3, 8);
      break;
    case 0x23:
      status_handle();
      if (n >= 13) {
        F("Page", sf("%u of %u", p[3], p[4]), p + 3, 2);
        F("Features", hexbytes(p + 5, 8), p + 5, 8);
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
        F("Key type", std::to_string(p[22]), p + 22, 1);
        s += " " + bdaddr_str(p) + sf(" type %u", p[22]);
      }
      break;
    case 0x32:  // IO Capability Response
      if (n >= 9) {
        F("Address", bdaddr_str(p), p, 6);
        F("IO capability", io_cap_name(p[6]), p + 6, 1);
        F("OOB data", std::to_string(p[7]), p + 7, 1);
        F("Authentication", sf("0x%02x", p[8]), p + 8, 1);
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
          F(sf("Response %zu", i), sf("%s class 0x%06x RSSI %d", bdaddr_str(q).c_str(), le24(q + 8),
                                      static_cast<int8_t>(q[13])), q, 14);
        } else {
          F(sf("Response %zu", i), sf("%s class 0x%06x", bdaddr_str(q).c_str(), le24(q + 9)), q, 14);
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
        F("Air mode", std::to_string(p[16]), p + 16, 1);
        s += p[0] ? " " + bdaddr_str(p + 3) + " failed: " + status_str(p[0])
                  : sf(" handle %u %s (%s)", le16(p + 1) & 0x0fff, bdaddr_str(p + 3).c_str(),
                       p[9] == 2 ? "eSCO" : "SCO");
      }
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
      if (n) F("Data", hexbytes(p, n, 64), p, n);
      break;
    default:
      // The rest that start with status + handle.
      switch (code) {
        case 0x06: case 0x09: case 0x0a: case 0x0d: case 0x1c: case 0x1d: case 0x21:
        case 0x2d: case 0x2e: case 0x30:
          status_handle();
          break;
        default:
          break;
      }
      if (o.detail && n) F("Parameters", hexbytes(p, n, 64), p, n);
      break;
  }
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
            if (n > 4) F("Data", hexbytes(p + 4, n - 4), p + 4, n - 4);
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

std::string att(Out& o, const uint8_t* d, size_t n) {
  if (n < 1) return "ATT (empty)";
  const uint8_t op = d[0];
  const char* nm = att_op_name(op);
  std::string s = nm ? nm : sf("ATT opcode 0x%02x", op);
  OPEN("ATT", s, d, n);
  F("Opcode", sf("0x%02x (%s)", op, nm ? nm : "unknown"), d, 1);
  const uint8_t* p = d + 1;
  const size_t m = n - 1;
  auto value = [&](const uint8_t* v, size_t vn) {
    F("Value", hexbytes(v, vn, 64) + (mostly_text(v, vn) ? "  \"" + printable(v, vn) + "\"" : ""), v, vn);
    return " " + hexbytes(v, vn, 16) + (vn && mostly_text(v, vn) ? " \"" + printable(v, vn, 24) + "\"" : "");
  };
  switch (op) {
    case 0x01:
      if (m >= 4) {
        const char* req = att_op_name(p[0]);
        F("Request", sf("0x%02x (%s)", p[0], req ? req : "?"), p, 1);
        F("Handle", sf("0x%04x", le16(p + 1)), p + 1, 2);
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
    case 0x09:  // Read By Type Response
    case 0x11: {  // Read By Group Type Response
      if (m < 1 || p[0] < 2) break;
      const size_t el = p[0];
      F("Length", std::to_string(el), p, 1);
      size_t cnt = 0;
      for (size_t i = 1; i + el <= m; i += el, ++cnt) {
        const uint8_t* e = p + i;
        if (op == 0x11 && el >= 6) {
          F(sf("0x%04x-0x%04x", le16(e), le16(e + 2)), uuid_str(e + 4, el - 4), e, el);
          if (cnt == 0) s += sf(" 0x%04x-0x%04x %s", le16(e), le16(e + 2), uuid_str(e + 4, el - 4).c_str());
        } else {
          F(sf("Handle 0x%04x", le16(e)), hexbytes(e + 2, el - 2, 64), e, el);
          if (cnt == 0) s += sf(" 0x%04x:", le16(e)) + " " + hexbytes(e + 2, el - 2, 12);
        }
      }
      if (cnt > 1) s += sf(" (+%zu more)", cnt - 1);
      break;
    }
    case 0x0a:  // Read Request
      if (m >= 2) {
        F("Handle", sf("0x%04x", le16(p)), p, 2);
        s += sf(" handle 0x%04x", le16(p));
      }
      break;
    case 0x0c:  // Read Blob Request
      if (m >= 4) {
        F("Handle", sf("0x%04x", le16(p)), p, 2);
        F("Offset", std::to_string(le16(p + 2)), p + 2, 2);
        s += sf(" handle 0x%04x offset %u", le16(p), le16(p + 2));
      }
      break;
    case 0x0b:
    case 0x0d:
    case 0x0f:
    case 0x21:
      s += value(p, m);
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
        F("Handle", sf("0x%04x", le16(p)), p, 2);
        s += sf(" handle 0x%04x", le16(p));
        const size_t vn = op == 0xd2 && m >= 14 ? m - 14 : m - 2;
        s += value(p + 2, vn);
        if (op == 0xd2 && m >= 14) F("Signature", hexbytes(p + 2 + vn, 12), p + 2 + vn, 12);
      }
      break;
    case 0x16:  // Prepare Write
    case 0x17:
      if (m >= 4) {
        F("Handle", sf("0x%04x", le16(p)), p, 2);
        F("Offset", std::to_string(le16(p + 2)), p + 2, 2);
        s += sf(" handle 0x%04x offset %u", le16(p), le16(p + 2));
        s += value(p + 4, m - 4);
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
        F(sf("Handle 0x%04x", le16(p + i)), hexbytes(p + i + 4, vl, 64), p + i, 4 + vl);
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
  if (o.detail && m && mtype != 0 && signal != 0x01 && signal != 0x02 && signal != 0x04 && signal != 0x0c) {
    F("Parameters", hexbytes(p, m, 64), p, m);
  }
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
  if (o.detail && pdu != 0x71 && pdu != 0x73 && pdu != 0x70 && n) F("Parameters", hexbytes(v, n, 64), v, n);
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
      const std::string d = avrcp_vendor_params(o, pdu, rsp, ctype, v, vn);
      if (pt == 1) s += " (start)";
      // "RegisterNotification CHANGED VOLUME_CHANGED: 87 (69%)": the event reads as part of the name.
      if (!d.empty()) s += (pdu == 0x31 && ctype != 0xa ? " " : ": ") + d;
      if (o.detail && vn && pdu != 0x20 && pdu != 0x31) F("Parameters", hexbytes(v, vn, 64), v, vn);
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

void acl(Out& o, const IndexEntry& e, const uint8_t* d, size_t len) {
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
    case kProtoAtt: s = att(o, q, m); break;
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
  F("Packet boundary", std::to_string(pb), d + 1, 1);
  F("Data length", std::to_string(dl), d + 2, 2);
  std::string s = sf("ISO %zu bytes", len - 4);
  size_t at = 4;
  if (pb == 0 || pb == 2) {  // a first (or complete) fragment carries the SDU header
    if (tsf && at + 4 <= len) {
      F("Timestamp", sf("%u us", le32(d + at)), d + at, 4);
      at += 4;
    }
    if (at + 4 <= len) {
      F("Sequence number", std::to_string(le16(d + at)), d + at, 2);
      F("SDU length", std::to_string(le16(d + at + 2) & 0x0fff), d + at + 2, 2);
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

void dissect(const IndexEntry& e, const uint8_t* d, size_t len, bool detail, Dissection* out) {
  Out o(d, detail);
  switch (e.mon) {
    case kMonCommand: hci_command(o, d, len); break;
    case kMonEvent: hci_event(o, d, len); break;
    case kMonAclTx:
    case kMonAclRx: acl(o, e, d, len); break;
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
