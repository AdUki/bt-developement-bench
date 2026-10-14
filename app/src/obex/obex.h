#pragma once

// The OBEX tester (docs/api.md "OBEX"): BlueZ's obexd driven over D-Bus, as a client — Object Push
// (OPP), File Transfer (FTP), Phonebook Access (PBAP), Message Access (MAP) and the AVRCP cover-art
// image pull (BIP) — and as a server, receiving what devices push or put, with an agent that
// accepts or asks.
//
// obexd is BlueZ's own OBEX implementation; this drives it rather than speaking OBEX itself, the
// same way the rest of btbenchd drives bluetoothd. Its API normally lives on the session bus,
// which a headless board does not have: btbenchd starts it with --system-bus (BlueZ's obex.conf
// lets root own org.bluez.obex there) unless something already owns the name (a desktop's obexd,
// tools/fake-bluez), and stops what it started when the daemon exits. Running obexd registers the
// board's OPP/FTP/PBAP/MAP server records with bluetoothd, so it is started on demand — the first
// OBEX request, or PUT /api/obex {"running": true} — and not at boot.
//
// One thread owns the D-Bus connection (sd-bus is not thread-safe); HTTP handlers queue calls and
// wait for the answer, since an OBEX operation's result (a folder listing, a phonebook) is the
// point. Transfers run in obexd; their progress arrives as PropertiesChanged and goes out on the
// "obex" topic (at most 4 Hz, and only while someone is subscribed).
//
// Files: everything lives under the root (default /data/btbench/obex), which is also obexd's root
// (it writes nothing outside it): out/ holds what is sent (uploaded through the API), in/ what is
// received or pulled, shared/ what is put there for others; a device browsing the board over FTP
// sees all three.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;

namespace btb::obex {

struct Options {
  std::string obexd = "/usr/libexec/bluetooth/obexd";
  std::string root = "/tmp/btbench/obex";  // RAM: transfers are not settings
  std::string state_file;                   // the server's policy, {"policy": "auto"}; "" keeps none
};

// The targets obexd's Client1.CreateSession takes, as the API names them.
bool target_ok(const std::string& t);
// A bare file name for the in/ and out/ folders: no separators, no leading dot, printable.
bool file_name_ok(const std::string& n);
// A transfer's object path → its id in the API ("/org/bluez/obex/client/session3/transfer7" →
// "session3/transfer7"; server transfers keep their "server/" prefix).
std::string transfer_id(const std::string& path);
// obexd's Transfer1 properties (as read_json gives them) → {id, path, session, status, name,
// filename, size, transferred, direction, ...} for the API.
nlohmann::json transfer_json(const std::string& path, const nlohmann::json& props);
// PBAP/MAP listings and FTP folder entries as the API serves them.
nlohmann::json folder_entries(const nlohmann::json& reply);       // ListFolder's aa{sv}
nlohmann::json vcard_list(const nlohmann::json& reply);           // List's a(ss)
nlohmann::json message_list(const nlohmann::json& reply);         // ListMessages' a{oa{sv}}
nlohmann::json image_properties(const nlohmann::json& reply);     // Image1.Properties' aa{sv}

struct CallResult {
  bool ok = false;
  bool timed_out = false;
  std::string error;       // obexd's message
  std::string error_name;  // org.bluez.obex.Error.*
  nlohmann::json reply = nlohmann::json::array();
};

class ObexManager {
 public:
  using Publish = std::function<void(const std::string& topic, const nlohmann::json& data)>;
  using HasSubscribers = std::function<bool(const std::string& topic)>;
  ObexManager(Options opt, Publish publish, HasSubscribers has_subscribers);
  ~ObexManager();
  ObexManager(const ObexManager&) = delete;
  ObexManager& operator=(const ObexManager&) = delete;

  // {running, managed, owner, error, root, agent:{policy, registered}, request, sessions:[...],
  //  transfers:[...], files:{in:[...], out:[...], shared:[...]}}
  nlohmann::json status();
  // Starts obexd (or adopts the one on the bus). Blocks up to a few seconds.
  bool start(std::string* err);
  void stop();  // stops obexd if btbenchd started it
  // obexd's --noplugin list (Setup's OBEX server profiles: "opp,ftp"); obexd started by btbenchd
  // and running is restarted with it.
  void set_noplugin(const std::string& list);

  // ---- client sessions ----
  // target: opp | ftp | pbap | map | bip-avrcp; opts: channel, psm (bip-avrcp needs the
  // player's ObexPort), source (the local adapter's address). Answers {id, path, target,
  // destination, ...}.
  bool create_session(const std::string& address, const std::string& target,
                      const nlohmann::json& opts, nlohmann::json* out, std::string* err, int* status);
  bool remove_session(const std::string& id, std::string* err, int* status);
  // One operation of a session's profile, e.g. {"op": "list"} on an FTP session; see docs/api.md
  // for the table. Waits for obexd's answer (a transfer's answer is the queued transfer).
  bool op(const std::string& session_id, const nlohmann::json& body, nlohmann::json* out,
          std::string* err, int* status);
  bool transfer_action(const std::string& id, const std::string& action, std::string* err, int* status);

  // ---- the server's agent ----
  // auto: accept every push into in/; ask: hold it for the console (answer()); reject.
  bool set_policy(const std::string& policy, std::string* err);
  bool answer(uint64_t id, bool accept, const std::string& name, std::string* err);

  // ---- files ----
  std::string path_of(const std::string& folder, const std::string& name) const;  // "" if bad
  bool folder_ok(const std::string& folder) const;
  const std::string& root() const { return opt_.root; }

  // Cover art: an AVRCP player's current image (BIP over its ObexPort), fetched once per handle
  // and kept under in/cover/. Returns the local file. `thumbnail` pulls the 200×200 JPEG every
  // BIP responder has, else the native image. `source` is the adapter the player is on: with two,
  // a session without one leaves from whichever the kernel routes it to, and finds no link there.
  bool cover_art(const std::string& address, const std::string& source, uint16_t psm, const std::string& handle,
                 bool thumbnail, std::string* file, std::string* err, int* status);
  // Opens the BIP channel to a player's ObexPort, if none is open. A phone puts image handles in
  // the track metadata only while one is (AVRCP 1.6): the handle comes with the next track info.
  bool cover_connect(const std::string& address, const std::string& source, uint16_t psm, std::string* err,
                     int* status);

  struct Bus;  // the bus thread's callbacks

 private:
  CallResult call(const std::string& path, const std::string& iface, const std::string& method,
                  std::function<int(sd_bus_message*)> append, int timeout_ms);
  void post(std::function<void(sd_bus*)> fn);
  void run();
  bool open_bus();
  void close_bus();
  void refresh_objects(sd_bus* bus);
  void on_signal(sd_bus_message* m);
  int agent_call(sd_bus_message* m, const char* member);
  void register_agent();
  void publish_soon();
  void tick();
  bool ensure_running(std::string* err);
  bool wait_for_owner(int timeout_ms);
  std::string session_path(const std::string& id) const;
  nlohmann::json files_json() const;

  const Options opt_;
  std::string noplugin_;  // under m_
  const Publish publish_;
  const HasSubscribers has_subscribers_;

  // Shared with HTTP handlers, under m_.
  mutable std::mutex m_;
  std::condition_variable owner_cv_;
  std::string owner_;  // org.bluez.obex's unique name, "" while nobody owns it
  bool managed_ = false;
  pid_t pid_ = -1;
  std::string error_;
  std::map<std::string, nlohmann::json> sessions_;   // by object path: Session1 properties
  std::map<std::string, nlohmann::json> transfers_;  // by object path: Transfer1 properties
  std::map<std::string, nlohmann::json> session_meta_;  // by object path: what we asked for
  std::string policy_ = "auto";
  bool agent_registered_ = false;
  struct Request {
    uint64_t id = 0;
    std::string transfer;  // its path
    std::string name;
    long long size = 0;
    std::string device;
    sd_bus_message* msg = nullptr;  // the AuthorizePush call, held until answered
    uint64_t deadline_ns = 0;
  };
  std::unique_ptr<Request> request_;
  uint64_t next_request_id_ = 1;
  bool cover_session(const std::string& address, const std::string& source, uint16_t psm, std::string* sid,
                     std::string* err, int* status);
  std::mutex cover_m_;

  // The bus thread.
  std::deque<std::function<void(sd_bus*)>> commands_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  int wake_fd_ = -1;
  sd_bus* bus_ = nullptr;
  sd_bus_slot* agent_slot_ = nullptr;
  std::vector<sd_bus_slot*> match_slots_;
  bool publish_pending_ = false;
  uint64_t last_publish_ns_ = 0;
};

}  // namespace btb::obex
