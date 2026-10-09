#pragma once

// UPnP/DLNA media servers as a source of audio: find them on the LAN (SSDP M-SEARCH), browse
// their ContentDirectory (SOAP Browse, DIDL-Lite), and hand an item's HTTP URL to a stream as an
// ordinary URL source. A control point only — the bench does not advertise itself as a renderer.
//
// The parsers (SSDP replies, device descriptions, the SOAP answer, DIDL-Lite) are pure functions
// over strings, unit-tested; UpnpBrowser does the network part with plain sockets and httplib's
// client. Media servers speak plain HTTP on the LAN, so the client needs no TLS.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace btb::audio {

// ---- a small, tolerant XML reader ---------------------------------------------------------------

// Enough XML for device descriptions, SOAP envelopes and DIDL-Lite: elements, attributes, text,
// CDATA, comments and processing instructions skipped, the five entities and numeric references
// decoded. Namespace prefixes are dropped from element and attribute names ("dc:title" → "title",
// "s:Body" → "Body"): no document here reuses a local name across namespaces in a way that
// matters. Malformed input yields whatever was parsed up to the fault, never an exception.
struct XmlNode {
  std::string name;
  std::map<std::string, std::string> attrs;
  std::string text;  // the element's own character data, concatenated
  std::vector<XmlNode> children;

  const XmlNode* child(const std::string& n) const;
  std::vector<const XmlNode*> all(const std::string& n) const;
  // The text of the first child called n ("" when there is none).
  std::string child_text(const std::string& n) const;
  // Depth-first search for the first descendant called n.
  const XmlNode* find(const std::string& n) const;
};
XmlNode parse_xml(const std::string& doc);
std::string xml_unescape(const std::string& s);
std::string xml_escape(const std::string& s);

// ---- SSDP and descriptions ----------------------------------------------------------------------

struct SsdpReply {
  std::string location;  // the device description's URL
  std::string usn;       // uuid:...::urn:...  — the server's identity
  std::string st;
  std::string server;
};
// An HTTP/1.1 200 OK reply to M-SEARCH (headers are case-insensitive). False when it is not one
// or has no LOCATION.
bool parse_ssdp_reply(const std::string& msg, SsdpReply* out);
std::string ssdp_msearch(const std::string& st, int mx_s);

struct MediaServer {
  std::string id;  // the device UDN ("uuid:...")
  std::string name, manufacturer, model;
  std::string location;
  std::string control_url;  // ContentDirectory's, absolute
  std::string icon_url;     // absolute, or ""
};
// A device description (the XML at LOCATION) → the first device in it, or nested in its
// deviceList, that has a ContentDirectory service. Relative URLs resolve against URLBase or the
// location. False when there is none.
bool parse_description(const std::string& xml, const std::string& location, MediaServer* out);
// `rel` against `base` the way a browser would (absolute, host-relative "/x", or path-relative).
std::string resolve_url(const std::string& base, const std::string& rel);
// "http://host:port/path?q" → scheme://host:port and /path?q. False for anything but http(s).
bool split_url(const std::string& url, std::string* origin, std::string* path);

// ---- ContentDirectory ---------------------------------------------------------------------------

std::string soap_browse_body(const std::string& object_id, bool metadata, unsigned start,
                             unsigned count);
// The SOAP answer → the DIDL-Lite document it carries (unescaped), NumberReturned and
// TotalMatches. A SOAP fault sets *fault to its description and returns false.
bool parse_browse_response(const std::string& xml, std::string* didl, unsigned* returned,
                           unsigned* total, std::string* fault);
// DIDL-Lite → {containers:[{id, title, child_count|null, class}], items:[{id, title, artist,
// album, class, duration_s|null, url, mime, size|null, art}]}. An item's url is its first <res>
// with an audio/* protocolInfo (or the first <res> at all when none says audio).
nlohmann::json parse_didl(const std::string& didl);
// "0:03:25.500" / "03:25" → seconds; -1 when it does not parse.
double parse_duration(const std::string& s);

// ---- the network side ---------------------------------------------------------------------------

// Finds media servers and browses them. search() blocks for its timeout (one M-SEARCH on every
// local IPv4 interface, then the descriptions fetched); browse() for one SOAP round trip. Both run
// on an HTTP worker of the request that asked: there is no background thread and no periodic
// search, so an unused feature costs the board nothing.
class UpnpBrowser {
 public:
  // {servers:[{id, name, manufacturer, model, location, icon}], searched_ms, error}
  nlohmann::json search(int timeout_ms);
  // The last search's result, without searching.
  nlohmann::json servers() const;
  // {containers, items, returned, total, object}; false with *err (a 404 for an unknown server).
  bool browse(const std::string& server_id, const std::string& object_id, unsigned start,
              unsigned count, nlohmann::json* out, std::string* err, int* status);

 private:
  mutable std::mutex m_;
  std::map<std::string, MediaServer> servers_;
  int64_t searched_ms_ = 0;
  std::string error_;
};

}  // namespace btb::audio
