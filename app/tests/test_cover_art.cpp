// The cover art responder's decisions: OBEX packets as a controller sends and reads them, the
// JPEG size its image properties carry, and the handles it gives image URLs. The L2CAP side needs
// Bluetooth; a controller (another board's Listen) exercises it.
#include "check.h"
#include "obex/cover_art.h"

using namespace btb;

namespace {

std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) out += std::string{d[c >> 4], d[c & 15]};
  return out;
}

void test_packets() {
  // A GET as obexd's BIP client sends it: Connection ID, Type (NUL-terminated), Img-Handle (UTF-16).
  const std::string get = obex::packet(0x83, "", {{0xCB, 1, ""}, {0x42, 0, std::string("x-bt/img-thm\0", 13)},
                                                   {0x30, 0, "1000001"}});
  CHECK_EQ(hex(get.substr(0, 3)), std::string("83002b"));  // 3 + 5 + 16 + 19
  CHECK_EQ(get.size(), size_t(0x2b));
  CHECK_EQ(hex(get.substr(get.size() - 4)), std::string("00310000"));  // "1" and the NUL, UTF-16BE
  std::vector<obex::Header> hs;
  CHECK(obex::parse_headers(reinterpret_cast<const uint8_t*>(get.data()) + 3, get.size() - 3, &hs));
  CHECK_EQ(hs.size(), size_t(3));
  CHECK_EQ(hs[0].num, uint32_t(1));
  CHECK_EQ(std::string(hs[1].bytes.c_str()), std::string("x-bt/img-thm"));
  CHECK_EQ(hs[2].bytes, std::string("1000001"));
  // CONNECT's answer: version, flags and size before the headers.
  const std::string ok = obex::packet(0xA0, std::string("\x10\x00\x7f\xff", 4), {{0xCB, 1, ""}});
  CHECK_EQ(hex(ok), std::string("a0000c10007fffcb00000001"));
  // A header running past the packet is refused.
  const uint8_t bad[] = {0x48, 0x00, 0x10, 0x01};
  CHECK(!obex::parse_headers(bad, sizeof bad, &hs));
}

void test_jpeg() {
  // SOI, an APP0 segment, then SOF0 with 200 lines of 300 pixels.
  std::string j("\xFF\xD8\xFF\xE0\x00\x04\x00\x00\xFF\xC0\x00\x11\x08\x00\xC8\x01\x2C\x03", 18);
  j += std::string(16, '\0');
  int w = 0, h = 0;
  CHECK(jpeg_size(j, &w, &h));
  CHECK_EQ(w, 300);
  CHECK_EQ(h, 200);
  CHECK(!jpeg_size("\x89PNG\r\n\x1a\n........", &w, &h));
  const std::string x = image_properties_xml("0000001", 300, 200, 1234);
  CHECK(x.find("handle=\"0000001\"") != std::string::npos);
  CHECK(x.find("pixel=\"300*200\" size=\"1234\"") != std::string::npos);
  CHECK(x.find("<variant encoding=\"JPEG\" pixel=\"200*200\"/>") != std::string::npos);
}

void test_handles() {
  CoverArtServer s("/tmp/btbench-cover-test");
  CHECK_EQ(s.handle_for(""), std::string(""));
  const std::string a = s.handle_for("http://img/a.jpg");
  CHECK_EQ(a, std::string("0000001"));
  CHECK_EQ(s.handle_for("http://img/b.jpg"), std::string("0000002"));
  CHECK_EQ(s.handle_for("http://img/a.jpg"), a);  // the same image keeps its handle
  CHECK(!s.connected());
  CHECK_EQ(s.psm(), uint16_t(0));  // not started
}

}  // namespace

int main() {
  test_packets();
  test_jpeg();
  test_handles();
  return report("test_cover_art");
}
