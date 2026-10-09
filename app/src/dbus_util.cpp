#include "dbus_util.h"

#include <errno.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include <algorithm>

#include "util/strings.h"

using json = nlohmann::json;

namespace btb {

int read_json(sd_bus_message* m, json* out) {
  char type = 0;
  const char* contents = nullptr;
  int r = sd_bus_message_peek_type(m, &type, &contents);
  if (r <= 0) return r;

  switch (type) {
    case SD_BUS_TYPE_BYTE: {
      uint8_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_BOOLEAN: {
      int v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v != 0;
      break;
    }
    case SD_BUS_TYPE_INT16: {
      int16_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_UINT16: {
      uint16_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_INT32: {
      int32_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_UINT32: {
      uint32_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_INT64: {
      int64_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_UINT64: {
      uint64_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_DOUBLE: {
      double v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_STRING:
    case SD_BUS_TYPE_OBJECT_PATH:
    case SD_BUS_TYPE_SIGNATURE: {
      const char* v = nullptr;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v ? v : "";
      break;
    }
    case SD_BUS_TYPE_UNIX_FD: {
      int v = -1;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = nullptr;
      break;
    }
    case SD_BUS_TYPE_VARIANT: {
      if ((r = sd_bus_message_enter_container(m, type, contents)) < 0) return r;
      if ((r = read_json(m, out)) < 0) return r;
      r = sd_bus_message_exit_container(m);
      break;
    }
    case SD_BUS_TYPE_ARRAY: {
      // A byte array in one read: a GATT value or an advertising payload is read as a whole, not
      // a byte at a time through peek/read pairs.
      if (contents[0] == SD_BUS_TYPE_BYTE && contents[1] == 0) {
        const void* p = nullptr;
        size_t n = 0;
        if ((r = sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &p, &n)) < 0) return r;
        const uint8_t* b = static_cast<const uint8_t*>(p);
        *out = json::array();
        for (size_t i = 0; i < n; ++i) out->push_back(b[i]);
        break;
      }
      if ((r = sd_bus_message_enter_container(m, type, contents)) < 0) return r;
      if (contents[0] == SD_BUS_TYPE_DICT_ENTRY_BEGIN) {
        *out = json::object();
        const std::string entry(contents + 1, strlen(contents) - 2);
        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, entry.c_str())) > 0) {
          json k, v;
          if ((r = read_json(m, &k)) < 0 || (r = read_json(m, &v)) < 0) return r;
          (*out)[k.is_string() ? k.get<std::string>() : k.dump()] = std::move(v);
          if ((r = sd_bus_message_exit_container(m)) < 0) return r;
        }
      } else {
        *out = json::array();
        for (;;) {
          json v;
          if ((r = read_json(m, &v)) <= 0) break;
          out->push_back(std::move(v));
        }
      }
      if (r < 0) return r;
      r = sd_bus_message_exit_container(m);
      break;
    }
    case SD_BUS_TYPE_STRUCT: {
      if ((r = sd_bus_message_enter_container(m, type, contents)) < 0) return r;
      *out = json::array();
      for (;;) {
        json v;
        if ((r = read_json(m, &v)) <= 0) break;
        out->push_back(std::move(v));
      }
      if (r < 0) return r;
      r = sd_bus_message_exit_container(m);
      break;
    }
    default:
      return -EINVAL;
  }
  return r < 0 ? r : 1;
}

int append_bytes(sd_bus_message* m, const std::vector<uint8_t>& b) {
  return sd_bus_message_append_array(m, 'y', b.data(), b.size());
}

int append_variant(sd_bus_message* m, const DVar& v) {
  int r = 0;
  switch (v.type) {
    case 's': return sd_bus_message_append(m, "v", "s", v.s.c_str());
    case 'b': return sd_bus_message_append(m, "v", "b", static_cast<int>(v.i != 0));
    case 'q': return sd_bus_message_append(m, "v", "q", static_cast<uint16_t>(v.i));
    case 'n': return sd_bus_message_append(m, "v", "n", static_cast<int16_t>(v.i));
    case 'u': return sd_bus_message_append(m, "v", "u", static_cast<uint32_t>(v.i));
    case 'i': return sd_bus_message_append(m, "v", "i", static_cast<int32_t>(v.i));
    case 'y': return sd_bus_message_append(m, "v", "y", static_cast<uint8_t>(v.i));
    case 'A':
      if ((r = sd_bus_message_open_container(m, 'v', "as")) < 0) return r;
      if ((r = sd_bus_message_open_container(m, 'a', "s")) < 0) return r;
      for (const std::string& s : v.as)
        if ((r = sd_bus_message_append(m, "s", s.c_str())) < 0) return r;
      if ((r = sd_bus_message_close_container(m)) < 0) return r;
      return sd_bus_message_close_container(m);
    case 'Y':
      if ((r = sd_bus_message_open_container(m, 'v', "ay")) < 0) return r;
      if ((r = append_bytes(m, v.ay)) < 0) return r;
      return sd_bus_message_close_container(m);
    default:
      return -EINVAL;
  }
}

int append_dict(sd_bus_message* m, const DDict& d) {
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
  for (const auto& kv : d) {
    if (r >= 0) r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", kv.first.c_str());
    if (r >= 0) r = append_variant(m, kv.second);
    if (r >= 0) r = sd_bus_message_close_container(m);
  }
  if (r >= 0) r = sd_bus_message_close_container(m);
  return r;
}

bool is_error(const sd_bus_error* e, const char* name) {
  return e && e->name && strcmp(e->name, name) == 0;
}

std::string friendly(const sd_bus_error* e) {
  if (!e || !e->name) return "failed";
  const std::string n = e->name;
  const std::string msg = e->message ? e->message : "";
  auto is = [&](const char* suffix) { return n == std::string("org.bluez.Error.") + suffix; };
  // BlueZ's message is the most precise thing there is (br-connection-page-timeout, Page Timeout,
  // le-connection-abort-by-local); it is kept, with what the name means in front where the
  // message alone is cryptic.
  std::string what;
  if (is("AuthenticationFailed")) what = "authentication failed (wrong PIN/passkey, or the peer lost the bond)";
  else if (is("AuthenticationRejected")) what = "pairing rejected";
  else if (is("AuthenticationCanceled")) what = "pairing canceled";
  else if (is("AuthenticationTimeout")) what = "pairing timed out";
  else if (is("ConnectionAttemptFailed")) what = "connection attempt failed";
  else if (is("InProgress")) what = "already in progress";
  else if (is("AlreadyConnected")) what = "already connected";
  else if (is("AlreadyExists")) what = "already exists";
  else if (is("DoesNotExist")) what = "does not exist";
  else if (is("NotReady")) what = "the adapter is not ready (powered off?)";
  else if (is("NotAvailable")) what = "not available";
  else if (is("NotSupported")) what = "not supported";
  else if (is("NotPermitted")) what = "not permitted";
  else if (is("NotAuthorized")) what = "not authorized";
  else if (is("InvalidArguments")) what = "invalid arguments";
  else if (is("InvalidValueLength")) what = "invalid value length";
  else if (n == "org.freedesktop.DBus.Error.NoReply" || n == "org.freedesktop.DBus.Error.Timeout")
    return "no answer in time";
  else if (n == "org.freedesktop.DBus.Error.ServiceUnknown") return "BlueZ is not running";
  else if (n == "org.freedesktop.DBus.Error.UnknownObject") return "no such object (gone?)";
  if (what.empty()) return msg.empty() ? n : msg;
  if (msg.empty() || lower(msg) == lower(what)) return what;
  return what + ": " + msg;
}

std::string address_from_path(const std::string& path) {
  const size_t at = path.find("/dev_");
  if (at == std::string::npos || path.size() < at + 5 + 17) return {};
  std::string a = path.substr(at + 5, 17);
  std::replace(a.begin(), a.end(), '_', ':');
  return a;
}

std::string adapter_from_path(const std::string& path) {
  const std::string root = "/org/bluez/";
  if (path.compare(0, root.size(), root) != 0) return {};
  const size_t end = path.find('/', root.size());
  return path.substr(root.size(), end == std::string::npos ? std::string::npos : end - root.size());
}

std::string str_of(const json& o, const char* key) {
  const auto it = o.find(key);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool bool_of(const json& o, const char* key) {
  const auto it = o.find(key);
  return it != o.end() && it->is_boolean() && it->get<bool>();
}

long long num_of(const json& o, const char* key, long long fallback) {
  const auto it = o.find(key);
  return it != o.end() && it->is_number() ? it->get<long long>() : fallback;
}

std::vector<uint8_t> bytes_of(const json& o, const char* key) {
  std::vector<uint8_t> b;
  const auto it = o.find(key);
  if (it == o.end() || !it->is_array()) return b;
  for (const json& v : *it)
    if (v.is_number_unsigned()) b.push_back(static_cast<uint8_t>(v.get<unsigned>()));
  return b;
}

std::vector<std::string> strv_of(const json& o, const char* key) {
  std::vector<std::string> v;
  const auto it = o.find(key);
  if (it == o.end() || !it->is_array()) return v;
  for (const json& s : *it)
    if (s.is_string()) v.push_back(s.get<std::string>());
  return v;
}

}  // namespace btb
