// hci_dissect FILE.btsnoop [FILTER] [--detail] [--hex] — print the packet view of a capture: frame number,
// protocol and summary of every packet the filter (docs/monitor.md "Filter") matches, and with
// --detail the field tree under each, with --hex the packet bytes. A development tool for checking the dissector against
// real captures (compare with btmon -r FILE); not a test.

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "packet_view.h"

using nlohmann::json;

namespace {

void print_tree(const json& nodes, int depth) {
  for (const json& n : nodes) {
    std::printf("%*s%s%s%s\n", 8 + depth * 2, "", n.value("name", std::string()).c_str(),
                n.contains("value") ? ": " : "", n.value("value", std::string()).c_str());
    if (n.contains("children")) print_tree(n["children"], depth + 1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string path, filter;
  bool detail = false, hex = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--detail") == 0) detail = true;
    else if (std::strcmp(argv[i], "--hex") == 0) hex = true;
    else if (path.empty()) path = argv[i];
    else filter = argv[i];
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: %s FILE.btsnoop [FILTER] [--detail] [--hex]\n", argv[0]);
    return 2;
  }
  btb::hci::PacketIndex idx;
  std::lock_guard<std::mutex> lk(idx.mu);
  std::string err;
  if (!idx.open(path, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  idx.extend(INT64_MAX);
  btb::hci::Filter f;
  if (!btb::hci::Filter::parse(filter, &f, &err)) {
    std::fprintf(stderr, "filter: %s\n", err.c_str());
    return 2;
  }
  const auto* q = idx.query(f, INT64_MAX);
  for (uint32_t i : q->matches) {
    const auto& e = idx.at(i);
    std::printf("%6u %-6s %s %s\n", i + 1, btb::hci::proto_name(e.proto),
                e.flags & btb::hci::kFlagTx ? ">" : e.flags & btb::hci::kFlagRx ? "<" : " ",
                idx.summary(i).c_str());
    if (!detail && !hex) continue;
    size_t len = 0;
    const uint8_t* d = idx.read(i, &len);
    if (!d) continue;
    if (hex) {
      std::printf("        ");
      for (size_t k = 0; k < len; ++k) std::printf("%02x", d[k]);
      std::printf("\n");
    }
    if (!detail) continue;
    // ATT values decode by their characteristic, from the discovery before them (as the console).
    btb::hci::GattDb gatt;
    if (e.proto == btb::hci::kProtoAtt) {
      const std::vector<uint8_t> bytes(d, d + len);
      btb::hci::gatt_before(idx, i, &gatt);
      btb::hci::Dissection ds;
      btb::hci::dissect(e, bytes.data(), bytes.size(), true, &ds, &gatt);
      print_tree(ds.fields, 0);
      continue;
    }
    btb::hci::Dissection ds;
    btb::hci::dissect(e, d, len, true, &ds);
    print_tree(ds.fields, 0);
  }
  return 0;
}
