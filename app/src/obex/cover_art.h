#pragma once

// The board's AVRCP cover art (docs/audio.md "The board's player"): a BIP Imaging Responder for
// the Cover Art target, so a controller showing what the board plays (a car, a speaker with a
// screen, another board's Listen) shows its cover as well, the way a phone's appears on the board.
//
// What a phone does as AVRCP target, the board does in three parts. BlueZ's target record offers
// cover art at this server's PSM (the player registers with ObexPort; the bench's BlueZ series);
// each track's metadata carries an image handle (bluez:ImgHandle), sent only while a controller
// is connected here, as AVRCP 1.6 asks; and this server answers the controller's GETs over OBEX:
// the image (x-bt/img-img), its 200×200 thumbnail (x-bt/img-thm) and its properties
// (x-bt/img-properties).
//
// It speaks just that much OBEX itself, over an L2CAP socket in ERTM (obexd has no imaging
// responder, and the board's player is btbenchd's anyway): CONNECT to the Cover Art target,
// GET, ABORT, DISCONNECT; no SRM (a client asking for it goes on without). A handle stands for an
// image URL (a station's cover, ICY StreamUrl), fetched once with curl when first asked for; the
// thumbnail is made with ffmpeg, or is the image itself without it.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace btb {

namespace obex {

// One OBEX header: its id, and its value (a 1- or 4-byte number in `num`, else the bytes; a
// Unicode header's text as UTF-8).
struct Header {
  uint8_t id = 0;
  uint32_t num = 0;
  std::string bytes;
};
// A request's headers, after its opcode, length and (CONNECT's) fixed fields; false when they do
// not add up.
bool parse_headers(const uint8_t* p, size_t n, std::vector<Header>* out);
// A packet: opcode, length, `fixed` (CONNECT's version, flags and size), then the headers. Unicode
// headers (ids 0x00-0x3f) take UTF-8 text and are sent as UTF-16BE with a terminating NUL.
std::string packet(uint8_t opcode, const std::string& fixed, const std::vector<Header>& headers);

}  // namespace obex

// A JPEG's size from its SOF marker; false when it is not one.
bool jpeg_size(const std::string& data, int* w, int* h);
// BIP's image properties of one image: its native encoding, size and pixels, and the 200×200
// JPEG thumbnail variant.
std::string image_properties_xml(const std::string& handle, int w, int h, size_t size);

class CoverArtServer {
 public:
  // `dir` keeps the images fetched, for this run only (start() empties it).
  explicit CoverArtServer(std::string dir);
  ~CoverArtServer();
  CoverArtServer(const CoverArtServer&) = delete;
  CoverArtServer& operator=(const CoverArtServer&) = delete;

  // Listens on a dynamic PSM on every adapter; false (and psm() 0) without Bluetooth sockets.
  bool start(std::string* err);
  void stop();
  uint16_t psm() const { return psm_; }

  // The handle for an image URL ("0000001"...), "" for none. The same URL keeps its handle.
  std::string handle_for(const std::string& url);
  // Whether a controller is connected (an OBEX CONNECT done): only then are handles sent.
  bool connected() const { return sessions_ > 0; }
  // Called (on the server's thread) when connected() changes, so the track is told again.
  void on_connected_change(std::function<void()> f) { on_change_ = std::move(f); }

 private:
  void accept_loop();
  void serve(int fd, std::string peer);
  // The image of a handle, fetched (and its thumbnail made) if not yet: false when it cannot be had.
  bool image(const std::string& handle, bool thumbnail, std::string* data, std::string* err);

  const std::string dir_;
  int listen_fd_ = -1;
  uint16_t psm_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread accept_;
  std::mutex m_;
  std::set<int> conns_;                   // open connections, shut down by stop()
  std::vector<std::thread> threads_;
  std::map<std::string, std::string> urls_;     // handle → URL
  std::map<std::string, std::string> handles_;  // URL → handle
  unsigned next_ = 1;
  std::mutex fetch_m_;  // one fetch at a time
  std::atomic<int> sessions_{0};
  std::function<void()> on_change_;
};

}  // namespace btb
