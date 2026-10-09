#include "audio/upnp.h"

#include <arpa/inet.h>
#include <httplib.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "util/log.h"
#include "util/strings.h"

using json = nlohmann::json;

namespace btb::audio {

namespace {

constexpr const char* kContentDirectory = "urn:schemas-upnp-org:service:ContentDirectory:1";
constexpr const char* kMediaServer = "urn:schemas-upnp-org:device:MediaServer:1";

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string local_name(const std::string& n) {
  const size_t c = n.find(':');
  return c == std::string::npos ? n : n.substr(c + 1);
}

void append_utf8(std::string* out, unsigned long cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x110000) {
    out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

// A recursive-descent reader over [pos, end). Depth is capped: a hostile server must not be able
// to blow the stack of an HTTP worker with a deeply nested reply.
class XmlReader {
 public:
  explicit XmlReader(const std::string& s) : s_(s) {}

  XmlNode document() {
    XmlNode root;
    root.name = "#document";
    content(&root, 0);
    return root;
  }

 private:
  bool at(const char* lit) const { return s_.compare(pos_, std::char_traits<char>::length(lit), lit) == 0; }

  void skip_past(const char* lit) {
    const size_t p = s_.find(lit, pos_);
    pos_ = p == std::string::npos ? s_.size() : p + std::char_traits<char>::length(lit);
  }

  // Children and text of `parent` until its end tag (or the end of input).
  void content(XmlNode* parent, int depth) {
    while (pos_ < s_.size()) {
      if (s_[pos_] != '<') {
        const size_t lt = s_.find('<', pos_);
        const size_t end = lt == std::string::npos ? s_.size() : lt;
        parent->text += xml_unescape(s_.substr(pos_, end - pos_));
        pos_ = end;
        continue;
      }
      if (at("<!--")) {
        skip_past("-->");
      } else if (at("<![CDATA[")) {
        pos_ += 9;
        const size_t e = s_.find("]]>", pos_);
        parent->text += s_.substr(pos_, (e == std::string::npos ? s_.size() : e) - pos_);
        pos_ = e == std::string::npos ? s_.size() : e + 3;
      } else if (at("<?") || at("<!")) {
        skip_past(">");
      } else if (at("</")) {
        skip_past(">");
        return;
      } else {
        element(parent, depth);
      }
    }
  }

  void element(XmlNode* parent, int depth) {
    ++pos_;  // '<'
    XmlNode n;
    size_t p = pos_;
    while (p < s_.size() && !std::isspace(static_cast<unsigned char>(s_[p])) && s_[p] != '>' && s_[p] != '/') ++p;
    n.name = local_name(s_.substr(pos_, p - pos_));
    pos_ = p;
    bool empty = false;
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++pos_;
      } else if (c == '/') {
        empty = true;
        ++pos_;
      } else if (c == '>') {
        ++pos_;
        break;
      } else {
        // name="value" (or 'value')
        size_t e = pos_;
        while (e < s_.size() && s_[e] != '=' && s_[e] != '>' && !std::isspace(static_cast<unsigned char>(s_[e]))) ++e;
        const std::string an = local_name(s_.substr(pos_, e - pos_));
        pos_ = e;
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        std::string av;
        if (pos_ < s_.size() && s_[pos_] == '=') {
          ++pos_;
          while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
          if (pos_ < s_.size() && (s_[pos_] == '"' || s_[pos_] == '\'')) {
            const char q = s_[pos_++];
            const size_t qe = s_.find(q, pos_);
            av = xml_unescape(s_.substr(pos_, (qe == std::string::npos ? s_.size() : qe) - pos_));
            pos_ = qe == std::string::npos ? s_.size() : qe + 1;
          }
        }
        if (!an.empty()) n.attrs[an] = av;
        empty = false;
      }
    }
    if (!empty) {
      if (depth < 64) content(&n, depth + 1);
      else skip_past(("</" + n.name).c_str());
    }
    parent->children.push_back(std::move(n));
  }

  const std::string& s_;
  size_t pos_ = 0;
};

std::string header(const std::string& msg, const std::string& name) {
  for (const std::string& line : split(msg, '\n')) {
    const size_t c = line.find(':');
    if (c == std::string::npos) continue;
    if (lower(trim(line.substr(0, c))) == name) return trim(line.substr(c + 1));
  }
  return {};
}

}  // namespace

// ---- XmlNode ------------------------------------------------------------------------------------

const XmlNode* XmlNode::child(const std::string& n) const {
  for (const XmlNode& c : children)
    if (c.name == n) return &c;
  return nullptr;
}

std::vector<const XmlNode*> XmlNode::all(const std::string& n) const {
  std::vector<const XmlNode*> out;
  for (const XmlNode& c : children)
    if (c.name == n) out.push_back(&c);
  return out;
}

std::string XmlNode::child_text(const std::string& n) const {
  const XmlNode* c = child(n);
  return c ? trim(c->text) : std::string{};
}

const XmlNode* XmlNode::find(const std::string& n) const {
  for (const XmlNode& c : children) {
    if (c.name == n) return &c;
    if (const XmlNode* d = c.find(n)) return d;
  }
  return nullptr;
}

XmlNode parse_xml(const std::string& doc) { return XmlReader(doc).document(); }

std::string xml_unescape(const std::string& s) {
  if (s.find('&') == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      continue;
    }
    const size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out.push_back('&');
      continue;
    }
    const std::string e = s.substr(i + 1, semi - i - 1);
    if (e == "lt") out += '<';
    else if (e == "gt") out += '>';
    else if (e == "amp") out += '&';
    else if (e == "quot") out += '"';
    else if (e == "apos") out += '\'';
    else if (!e.empty() && e[0] == '#') {
      const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
      char* endp = nullptr;
      const unsigned long cp = std::strtoul(e.c_str() + (hex ? 2 : 1), &endp, hex ? 16 : 10);
      if (endp && *endp == '\0') append_utf8(&out, cp);
    } else {
      out += s.substr(i, semi - i + 1);  // an entity we do not know: kept as written
    }
    i = semi;
  }
  return out;
}

std::string xml_escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

// ---- SSDP, descriptions -------------------------------------------------------------------------

bool parse_ssdp_reply(const std::string& msg, SsdpReply* out) {
  if (!starts_with(msg, "HTTP/1.1 200") && !starts_with(msg, "HTTP/1.0 200")) return false;
  out->location = header(msg, "location");
  out->usn = header(msg, "usn");
  out->st = header(msg, "st");
  out->server = header(msg, "server");
  return !out->location.empty();
}

std::string ssdp_msearch(const std::string& st, int mx_s) {
  return "M-SEARCH * HTTP/1.1\r\n"
         "HOST: 239.255.255.250:1900\r\n"
         "MAN: \"ssdp:discover\"\r\n"
         "MX: " + std::to_string(mx_s) + "\r\n"
         "ST: " + st + "\r\n"
         "USER-AGENT: Linux/1 UPnP/1.1 btbenchd/1\r\n\r\n";
}

bool split_url(const std::string& url, std::string* origin, std::string* path) {
  const std::string l = lower(url);
  size_t start = 0;
  if (starts_with(l, "http://")) start = 7;
  else if (starts_with(l, "https://")) start = 8;
  else return false;
  const size_t slash = url.find('/', start);
  *origin = url.substr(0, slash);
  *path = slash == std::string::npos ? "/" : url.substr(slash);
  return origin->size() > start;
}

std::string resolve_url(const std::string& base, const std::string& rel) {
  if (rel.empty()) return {};
  std::string origin, path;
  const std::string l = lower(rel);
  if (starts_with(l, "http://") || starts_with(l, "https://")) return rel;
  if (!split_url(base, &origin, &path)) return rel;
  if (rel[0] == '/') return origin + rel;
  const size_t q = path.find_first_of("?#");
  if (q != std::string::npos) path.resize(q);
  const size_t last = path.rfind('/');
  return origin + path.substr(0, last + 1) + rel;
}

bool parse_description(const std::string& xml, const std::string& location, MediaServer* out) {
  const XmlNode doc = parse_xml(xml);
  const XmlNode* root = doc.find("root");
  if (!root) return false;
  std::string base = root->child_text("URLBase");
  if (base.empty()) base = location;
  // The device itself, and the embedded ones (a NAS often nests its media server).
  std::vector<const XmlNode*> todo;
  if (const XmlNode* d = root->child("device")) todo.push_back(d);
  while (!todo.empty()) {
    const XmlNode* dev = todo.front();
    todo.erase(todo.begin());
    if (const XmlNode* sl = dev->child("serviceList")) {
      for (const XmlNode* svc : sl->all("service")) {
        if (svc->child_text("serviceType").find("ContentDirectory") == std::string::npos) continue;
        out->id = dev->child_text("UDN");
        out->name = dev->child_text("friendlyName");
        out->manufacturer = dev->child_text("manufacturer");
        out->model = dev->child_text("modelName");
        out->location = location;
        out->control_url = resolve_url(base, svc->child_text("controlURL"));
        out->icon_url.clear();
        if (const XmlNode* il = dev->child("iconList")) {
          for (const XmlNode* ic : il->all("icon")) {
            const std::string mime = ic->child_text("mimetype");
            if (out->icon_url.empty() || mime == "image/png")
              out->icon_url = resolve_url(base, ic->child_text("url"));
          }
        }
        if (out->id.empty()) out->id = location;
        return !out->control_url.empty();
      }
    }
    if (const XmlNode* dl = dev->child("deviceList"))
      for (const XmlNode* d : dl->all("device")) todo.push_back(d);
  }
  return false;
}

// ---- ContentDirectory ---------------------------------------------------------------------------

std::string soap_browse_body(const std::string& object_id, bool metadata, unsigned start,
                             unsigned count) {
  return std::string(
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
             "<u:Browse xmlns:u=\"") +
         kContentDirectory + "\"><ObjectID>" + xml_escape(object_id) + "</ObjectID><BrowseFlag>" +
         (metadata ? "BrowseMetadata" : "BrowseDirectChildren") +
         "</BrowseFlag><Filter>*</Filter><StartingIndex>" + std::to_string(start) +
         "</StartingIndex><RequestedCount>" + std::to_string(count) +
         "</RequestedCount><SortCriteria></SortCriteria></u:Browse></s:Body></s:Envelope>";
}

bool parse_browse_response(const std::string& xml, std::string* didl, unsigned* returned,
                           unsigned* total, std::string* fault) {
  const XmlNode doc = parse_xml(xml);
  if (const XmlNode* f = doc.find("Fault")) {
    std::string d = f->find("errorDescription") ? trim(f->find("errorDescription")->text) : "";
    const std::string code = f->find("errorCode") ? trim(f->find("errorCode")->text) : "";
    if (d.empty()) d = f->child_text("faultstring");
    *fault = "the server refused: " + (d.empty() ? std::string("SOAP fault") : d) +
             (code.empty() ? "" : " (UPnP error " + code + ")");
    return false;
  }
  const XmlNode* r = doc.find("BrowseResponse");
  if (!r) {
    *fault = "not a Browse answer";
    return false;
  }
  // Result is DIDL-Lite escaped into text; the reader has already unescaped it once.
  *didl = r->child("Result") ? r->child("Result")->text : std::string{};
  *returned = static_cast<unsigned>(std::strtoul(r->child_text("NumberReturned").c_str(), nullptr, 10));
  *total = static_cast<unsigned>(std::strtoul(r->child_text("TotalMatches").c_str(), nullptr, 10));
  return true;
}

double parse_duration(const std::string& s) {
  if (s.empty()) return -1;
  double total = 0;
  int parts = 0;
  for (const std::string& p : split(s, ':', false)) {
    char* end = nullptr;
    const double v = std::strtod(p.c_str(), &end);
    if (end == p.c_str()) return -1;
    total = total * 60 + v;
    ++parts;
  }
  return parts ? total : -1;
}

json parse_didl(const std::string& didl) {
  json containers = json::array(), items = json::array();
  const XmlNode doc = parse_xml(didl);
  const XmlNode* root = doc.find("DIDL-Lite");
  if (!root) return json{{"containers", containers}, {"items", items}};
  for (const XmlNode* c : root->all("container")) {
    const auto cc = c->attrs.find("childCount");
    containers.push_back(json{{"id", c->attrs.count("id") ? c->attrs.at("id") : ""},
                              {"title", c->child_text("title")},
                              {"class", c->child_text("class")},
                              {"child_count", cc == c->attrs.end() || cc->second.empty()
                                                  ? json(nullptr)
                                                  : json(std::strtoul(cc->second.c_str(), nullptr, 10))}});
  }
  for (const XmlNode* it : root->all("item")) {
    const XmlNode* res = nullptr;
    for (const XmlNode* r : it->all("res")) {
      const auto pi = r->attrs.find("protocolInfo");
      const std::string info = pi == r->attrs.end() ? "" : lower(pi->second);
      if (!res) res = r;
      if (info.find(":audio/") != std::string::npos) {
        res = r;
        break;
      }
    }
    std::string mime, url;
    json duration = nullptr, size = nullptr;
    if (res) {
      url = trim(res->text);
      const auto pi = res->attrs.find("protocolInfo");
      if (pi != res->attrs.end()) {
        // http-get:*:audio/mpeg:DLNA.ORG_PN=MP3 — the third field is the MIME type.
        const std::vector<std::string> f = split(pi->second, ':', false);
        if (f.size() >= 3) mime = f[2];
      }
      const auto du = res->attrs.find("duration");
      if (du != res->attrs.end()) {
        const double d = parse_duration(du->second);
        if (d >= 0) duration = d;
      }
      const auto sz = res->attrs.find("size");
      if (sz != res->attrs.end() && !sz->second.empty()) size = std::strtoull(sz->second.c_str(), nullptr, 10);
    }
    std::string artist = it->child_text("artist");
    if (artist.empty()) artist = it->child_text("creator");
    items.push_back(json{{"id", it->attrs.count("id") ? it->attrs.at("id") : ""},
                         {"title", it->child_text("title")},
                         {"artist", artist},
                         {"album", it->child_text("album")},
                         {"class", it->child_text("class")},
                         {"duration_s", duration},
                         {"url", url},
                         {"mime", mime},
                         {"size", size},
                         {"art", it->child_text("albumArtURI")}});
  }
  return json{{"containers", containers}, {"items", items}};
}

// ---- UpnpBrowser --------------------------------------------------------------------------------

namespace {

// One M-SEARCH out of every interface that has an IPv4 address (the gadget link, the Wi-Fi), and
// every unicast answer that comes back within the timeout.
std::vector<SsdpReply> ssdp_search(int timeout_ms, std::string* err) {
  std::vector<SsdpReply> out;
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *err = std::string("socket: ") + strerror(errno);
    return out;
  }
  sockaddr_in any{};
  any.sin_family = AF_INET;
  bind(fd, reinterpret_cast<sockaddr*>(&any), sizeof(any));
  const unsigned char ttl = 2;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);
  const int mx = std::max(1, timeout_ms / 1000);
  const std::string msgs[] = {ssdp_msearch(kMediaServer, mx), ssdp_msearch(kContentDirectory, mx)};
  int sent = 0;
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0) {
    for (ifaddrs* i = ifa; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
      if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST)) continue;
      const in_addr a = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr;
      if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a)) != 0) continue;
      for (const std::string& m : msgs) {
        if (sendto(fd, m.data(), m.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) > 0) ++sent;
      }
    }
    freeifaddrs(ifa);
  }
  if (!sent) {
    *err = "no network interface to search on";
    close(fd);
    return out;
  }
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[2048];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, static_cast<int>(left)) <= 0) continue;
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) continue;
    SsdpReply r;
    if (parse_ssdp_reply(std::string(buf, static_cast<size_t>(n)), &r)) out.push_back(r);
  }
  close(fd);
  return out;
}

bool http_fetch(const std::string& url, const std::string* soap_body, std::string* body,
                std::string* err) {
  std::string origin, path;
  if (!split_url(url, &origin, &path) || starts_with(lower(origin), "https")) {
    *err = "not an http URL: " + url;
    return false;
  }
  httplib::Client cli(origin);
  cli.set_connection_timeout(3, 0);
  cli.set_read_timeout(8, 0);
  cli.set_follow_location(true);
  httplib::Result r = soap_body
                          ? cli.Post(path, {{"SOAPACTION", std::string("\"") + kContentDirectory + "#Browse\""}},
                                     *soap_body, "text/xml; charset=\"utf-8\"")
                          : cli.Get(path);
  if (!r) {
    *err = origin + ": " + httplib::to_string(r.error());
    return false;
  }
  *body = r->body;
  // A SOAP fault comes back as a 500 with the fault in the body: let the caller read it.
  if (r->status != 200 && !(soap_body && r->status == 500)) {
    *err = origin + " answered HTTP " + std::to_string(r->status);
    return false;
  }
  return true;
}

json server_json(const MediaServer& s) {
  return json{{"id", s.id},         {"name", s.name},         {"manufacturer", s.manufacturer},
              {"model", s.model},   {"location", s.location}, {"icon", s.icon_url}};
}

}  // namespace

json UpnpBrowser::search(int timeout_ms) {
  std::string err;
  const std::vector<SsdpReply> replies = ssdp_search(timeout_ms, &err);
  std::map<std::string, MediaServer> found;
  std::vector<std::string> seen;
  for (const SsdpReply& r : replies) {
    if (std::find(seen.begin(), seen.end(), r.location) != seen.end()) continue;
    seen.push_back(r.location);
    std::string body, e;
    if (!http_fetch(r.location, nullptr, &body, &e)) {
      LOG_DEBUG("upnp: {}: {}", r.location, e);
      continue;
    }
    MediaServer s;
    if (parse_description(body, r.location, &s)) found[s.id] = s;
  }
  std::lock_guard<std::mutex> lk(m_);
  servers_ = std::move(found);
  searched_ms_ = wall_ms();
  error_ = err;
  json list = json::array();
  for (const auto& [id, s] : servers_) list.push_back(server_json(s));
  return json{{"servers", list}, {"searched_ms", searched_ms_}, {"error", error_}};
}

json UpnpBrowser::servers() const {
  std::lock_guard<std::mutex> lk(m_);
  json list = json::array();
  for (const auto& [id, s] : servers_) list.push_back(server_json(s));
  return json{{"servers", list}, {"searched_ms", searched_ms_ ? json(searched_ms_) : json(nullptr)},
              {"error", error_}};
}

bool UpnpBrowser::browse(const std::string& server_id, const std::string& object_id, unsigned start,
                         unsigned count, json* out, std::string* err, int* status) {
  MediaServer s;
  {
    std::lock_guard<std::mutex> lk(m_);
    const auto it = servers_.find(server_id);
    if (it == servers_.end()) {
      *err = "no such media server (search again)";
      *status = 404;
      return false;
    }
    s = it->second;
  }
  const std::string body = soap_browse_body(object_id, false, start, count);
  std::string reply;
  if (!http_fetch(s.control_url, &body, &reply, err)) {
    *status = 502;
    return false;
  }
  std::string didl;
  unsigned returned = 0, total = 0;
  if (!parse_browse_response(reply, &didl, &returned, &total, err)) {
    *status = 502;
    return false;
  }
  *out = parse_didl(didl);
  (*out)["returned"] = returned;
  (*out)["total"] = total;
  (*out)["object"] = object_id;
  (*out)["server"] = server_json(s);
  return true;
}

}  // namespace btb::audio
