#pragma once

// A link and its open L2CAP channels as the live Decoder knows them. The live packet view only
// tracks channels while someone watches (docs/monitor.md "Live packets"), so a viewer arriving
// mid-stream starts its Classifier from this snapshot: an A2DP stream that was opened an hour ago
// is still proto:rtp. Its own header because decoder.cpp and packet_view.cpp each keep private
// constants of the same names, and neither should have to see the other.

#include <cstdint>
#include <vector>

namespace btb::hci {

struct LinkSeed {
  uint16_t index = 0;
  uint16_t handle = 0;
  const char* type = "acl";  // acl | le | sco | esco | cis | bis (static strings)
  // TX packets still awaiting their NOCP. The next completions are theirs: without them in the
  // FIFO every latency after the seed would be measured from the packet sent after the right one.
  uint16_t in_flight = 0;
  struct Chan {
    uint16_t lcid = 0;  // ours: what RX packets carry
    uint16_t rcid = 0;  // the peer's: what TX packets carry
    uint16_t psm = 0;
    bool avdtp_sig = false;    // the link's AVDTP signalling channel
    bool avdtp_media = false;  // an AVDTP transport channel
  };
  std::vector<Chan> chans;  // dynamic channels with a known PSM only
};

}  // namespace btb::hci
