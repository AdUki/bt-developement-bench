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
#include <cstring>
#include <string>

#include "codec.h"
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
    case 0x0019: return "AVDTP";
    case 0x0100: return "L2CAP";
    case 0x1002: return "Public Browse Root";
    case 0x1101: return "Serial Port";
    case 0x1105: return "OBEX Object Push";
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
    case 0x111e: return "Handsfree";
    case 0x111f: return "Handsfree Audio Gateway";
    case 0x112f: return "Phonebook Access PSE";
    case 0x1132: return "Message Access Server";
    case 0x1200: return "PnP Information";
    case 0x1203: return "Generic Audio";
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
    case 0x2a24: return "Model Number String";
    case 0x2a29: return "Manufacturer Name String";
    case 0x2a37: return "Heart Rate Measurement";
    case 0x2a4d: return "Report";
    case 0x2aa6: return "Central Address Resolution";
    case 0x2b29: return "Client Supported Features";
    case 0x2b2a: return "Database Hash";
    case 0x2b3a: return "Server Supported Features";
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
        F("Class", sf("0x%06x", le24(p)), p, 3);
        s += sf(" 0x%06x", le24(p));
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
        F("Class", sf("0x%06x", le24(p + 6)), p + 6, 3);
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
        F("Class", sf("0x%06x", le24(p + 9)), p + 9, 3);
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
        F("Initiator keys", sf("0x%02x", p[4]), p + 4, 1);
        F("Responder keys", sf("0x%02x", p[5]), p + 5, 1);
        s += std::string(" ") + io_cap_name(p[0]) + " " + auth_req_str(p[2]);
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
  if (pdu == 0x01 && m >= 2) {
    F("Error", sf("0x%04x", be16(p)), p, 2);
    s += sf(" error 0x%04x", be16(p));
  } else if ((pdu == 0x02 || pdu == 0x06) && m) {
    std::string u;
    de_uuids(p, m, &u);
    if (!u.empty()) {
      F("Search pattern", u.substr(1), p, m);
      s += u;
    }
  } else if (pdu == 0x04 && m >= 4) {
    F("Record handle", sf("0x%08x", be32(p)), p, 4);
    s += sf(" record 0x%08x", be32(p));
  } else if ((pdu == 0x05 || pdu == 0x07) && m >= 2) {
    F("Byte count", std::to_string(be16(p)), p, 2);
    s += sf(" %u bytes", be16(p));
  } else if (pdu == 0x03 && m >= 4) {
    F("Total records", std::to_string(be16(p)), p, 2);
    F("Current records", std::to_string(be16(p + 2)), p + 2, 2);
    s += sf(" %u records", be16(p + 2));
  }
  if (o.detail && m) F("Parameters", hexbytes(p, m, 64), p, m);
  CLOSE();
  return s;
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
      const std::string t = printable(p, m, 120);
      F("Data", "\"" + t + "\"", p, m);
      s += ": " + t;
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

std::string avdtp_caps(Out& o, const uint8_t* p, size_t n) {
  std::string codec;
  while (n >= 2) {
    const uint8_t cat = p[0];
    const size_t cl = p[1];
    if (cl > n - 2) break;
    if (cat == 7) {
      CodecInfo ci;
      if (decode_media_codec(p + 2, cl, &ci)) {
        codec = ci.name + (ci.config.empty() ? "" : " " + ci.config);
        F("Media Codec", codec, p, 2 + cl);
      } else {
        F("Media Codec", hexbytes(p + 2, cl), p, 2 + cl);
      }
    } else {
      F(avdtp_category(cat), cl ? hexbytes(p + 2, cl) : "", p, 2 + cl);
      if (cat == 8) codec += codec.empty() ? "delay reporting" : ", delay reporting";
    }
    p += 2 + cl;
    n -= 2 + cl;
  }
  return codec;
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
        const std::string sep = sf("%u %s%s%s", p[i] >> 2, (p[i + 1] >> 4) == 0 ? "audio " : "",
                                   (p[i + 1] & 0x08) ? "sink" : "source", (p[i] & 0x02) ? " (in use)" : "");
        F(sf("SEP %u", p[i] >> 2), sep, p + i, 2);
        s += (i ? ", " : " ") + sep;
      }
    } else if (signal == 0x02 || signal == 0x0c || signal == 0x04) {
      const std::string c = avdtp_caps(o, p, m);
      if (!c.empty()) s += ": " + c;
    }
  } else if (mtype == 3) {
    const bool second = signal == 0x03 || signal == 0x05 || signal == 0x07 || signal == 0x09;
    const size_t at = second ? 1 : 0;
    if (second && m >= 1) F(signal == 0x07 || signal == 0x09 ? "ACP SEID" : "Category", std::to_string(signal == 0x07 || signal == 0x09 ? p[0] >> 2 : p[0]), p, 1);
    if (m > at) {
      F("Error", sf("0x%02x", p[at]), p + at, 1);
      s += sf(" (error 0x%02x)", p[at]);
    }
  }
  if (o.detail && m && mtype != 0 && signal != 0x01 && signal != 0x02 && signal != 0x0c) {
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

const char* passthrough_name(uint8_t op) {
  switch (op) {
    case 0x40: return "POWER";
    case 0x41: return "VOLUME_UP";
    case 0x42: return "VOLUME_DOWN";
    case 0x43: return "MUTE";
    case 0x44: return "PLAY";
    case 0x45: return "STOP";
    case 0x46: return "PAUSE";
    case 0x48: return "REWIND";
    case 0x49: return "FAST_FORWARD";
    case 0x4b: return "FORWARD";
    case 0x4c: return "BACKWARD";
    case 0x7e: return "VENDOR_UNIQUE";
    default: return nullptr;
  }
}

std::string avctp(Out& o, const uint8_t* d, size_t n, bool browsing) {
  if (n < 3) return "AVCTP (truncated)";
  const uint8_t label = d[0] >> 4, ptype = (d[0] >> 2) & 3;
  const bool rsp = d[0] & 0x02;
  OPEN("AVCTP", "", d, 3);
  F("Header", sf("label %u, %s, %s%s", label, ptype ? "fragmented" : "single",
                 rsp ? "response" : "command", d[0] & 1 ? ", invalid PID" : ""), d, 1);
  if (ptype != 0) {
    CLOSE();
    return sf("AVCTP fragment (label %u)", label);
  }
  F("PID", sf("0x%04x%s", be16(d + 1), be16(d + 1) == 0x110e ? " (A/V Remote Control)" : ""), d + 1, 2);
  CLOSE();
  const uint8_t* p = d + 3;
  const size_t m = n - 3;
  if (browsing) {
    if (m < 3) return "AVRCP browsing (truncated)";
    const char* nm = avrcp_pdu_name(p[0]);
    OPEN("AVRCP browsing", nm ? nm : "?", p, m);
    F("PDU", sf("0x%02x (%s)", p[0], nm ? nm : "?"), p, 1);
    F("Parameter length", std::to_string(be16(p + 1)), p + 1, 2);
    if (m > 3) F("Parameters", hexbytes(p + 3, m - 3, 64), p + 3, m - 3);
    CLOSE();
    return std::string(nm ? nm : sf("PDU 0x%02x", p[0]).c_str()) + (rsp ? " rsp" : "");
  }
  if (m < 3) return sf("AVCTP %s (label %u)", rsp ? "response" : "command", label);
  const uint8_t ctype = p[0] & 0x0f, opcode = p[2];
  OPEN("AV/C", avc_ctype_name(ctype), p, m);
  F("Type", sf("0x%x (%s)", ctype, avc_ctype_name(ctype)), p, 1);
  F("Subunit", sf("type 0x%02x, id %u", p[1] >> 3, p[1] & 7), p + 1, 1);
  std::string s;
  if (opcode == 0x7c && m >= 5) {  // PASS THROUGH
    const uint8_t op = p[3] & 0x7f;
    const char* on = passthrough_name(op);
    F("Opcode", "PASS THROUGH", p + 2, 1);
    F("Operation", sf("0x%02x (%s) %s", op, on ? on : "?", p[3] & 0x80 ? "released" : "pressed"), p + 3, 1);
    s = sf("PASS THROUGH %s %s %s", on ? on : sf("0x%02x", op).c_str(), p[3] & 0x80 ? "released" : "pressed",
           avc_ctype_name(ctype));
  } else if (opcode == 0x00 && m >= 10) {  // VENDOR DEPENDENT: company, PDU, packet type, length
    const uint8_t pdu = p[6];
    const char* pn = avrcp_pdu_name(pdu);
    F("Opcode", "VENDOR DEPENDENT", p + 2, 1);
    F("Company", sf("0x%06x", (p[3] << 16) | (p[4] << 8) | p[5]), p + 3, 3);
    F("PDU", sf("0x%02x (%s)", pdu, pn ? pn : "?"), p + 6, 1);
    F("Parameter length", std::to_string(be16(p + 8)), p + 8, 2);
    s = std::string(pn ? pn : sf("PDU 0x%02x", pdu).c_str()) + " " + avc_ctype_name(ctype);
    const uint8_t* v = p + 10;
    const size_t vn = m - 10;
    if (pdu == 0x31 && vn >= 1) {
      F("Event", avrcp_event_name(v[0]), v, 1);
      s += std::string(" (") + avrcp_event_name(v[0]) + ")";
      if (v[0] == 0x0d && vn >= 2 && rsp) s += sf(" volume %u", v[1] & 0x7f);
    } else if (pdu == 0x50 && vn >= 1) {
      F("Volume", sf("%u (%u%%)", v[0] & 0x7f, (v[0] & 0x7f) * 100 / 127), v, 1);
      s += sf(" %u", v[0] & 0x7f);
    } else if (pdu == 0x10 && vn >= 1) {
      F("Capability", v[0] == 2 ? "company IDs" : v[0] == 3 ? "events supported" : "?", v, 1);
    }
    if (vn) F("Parameters", hexbytes(v, vn, 64), v, vn);
  } else {
    F("Opcode", sf("0x%02x%s", opcode, opcode == 0x30 ? " (UNIT INFO)" : opcode == 0x31 ? " (SUBUNIT INFO)" : ""), p + 2, 1);
    s = sf("%s %s", opcode == 0x30 ? "UNIT INFO" : opcode == 0x31 ? "SUBUNIT INFO" : "AV/C", avc_ctype_name(ctype));
    if (m > 3) F("Operands", hexbytes(p + 3, m - 3), p + 3, m - 3);
  }
  CLOSE();
  return s;
}

// ---------------------------------------------------------------------------------------------
// ACL → L2CAP → the channel's protocol

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
        F("Data", hexbytes(q, m, 64), q, m);
        s = sf("CID 0x%04x, %zu bytes", cid, m);
      }
      break;
    case kProtoAtt: s = att(o, q, m); break;
    case kProtoSmp: s = smp(o, q, m); break;
    case kProtoSdp: s = sdp(o, q, m); break;
    case kProtoRfcomm: s = rfcomm(o, q, m); break;
    case kProtoAvdtp: s = avdtp(o, q, m); break;
    case kProtoRtp: s = rtp(o, q, m); break;
    case kProtoAvctp: s = avctp(o, q, m, e.psm == kPsmAvctpBrowsing); break;
    case kProtoBnep:
      F("BNEP", m ? sf("type 0x%02x", q[0] & 0x7f) : "", q, m);
      s = m ? sf("BNEP type 0x%02x, %zu bytes", q[0] & 0x7f, m) : "BNEP";
      break;
    case kProtoHid:
      F("HID", m ? sf("header 0x%02x", q[0]) : "", q, m);
      s = m ? sf("HID header 0x%02x, %zu bytes", q[0], m) : "HID";
      break;
    default:
      F("Data", hexbytes(q, m, 64), q, m);
      s = sf("CID 0x%04x, %zu bytes", cid, m);
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
      if (len >= 6) {
        const bool cmd = e.mon == kMonCtrlCommand;
        F("Cookie", sf("0x%08x", le32(d)), d, 4);
        F(cmd ? "Opcode" : "Event", sf("0x%04x", le16(d + 4)), d + 4, 2);
        if (len > 6) F("Data", hexbytes(d + 6, len - 6, 64), d + 6, len - 6);
        // MGMT Command Complete/Status carry the opcode they answer, then a status.
        if (!cmd && (le16(d + 4) == 0x0001 || le16(d + 4) == 0x0002) && len >= 9) {
          o.summary = sf("MGMT %s 0x%04x status 0x%02x", le16(d + 4) == 1 ? "Command Complete" : "Command Status",
                         le16(d + 6), d[8]);
        } else {
          o.summary = sf("MGMT %s 0x%04x", cmd ? "Command" : "Event", le16(d + 4));
        }
      } else {
        o.summary = "MGMT";
      }
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
