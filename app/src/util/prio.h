#pragma once

namespace btb {

// CPU priorities on a board with one core. Audio comes first: a stream's pump and the decoder and
// pw-cat it starts sit above WirePlumber's main loop (nice -11), whose Lua rescans after a volume
// change take a Zero W a quarter of a second — long enough to starve a nice-0 pw-cat into xruns.
// What only records or shows (graphs, the D-Bus log, the HCI monitor, pw-top, the journal view)
// sits below everything else: it can fall a second behind, the audio cannot.
constexpr int kAudioNice = -15;
constexpr int kBackgroundNice = 10;

// The calling thread's nice value (Linux keeps one per thread). Threads and processes it starts
// afterwards inherit it, which is how a pump's decoder and sink get theirs. Raising a priority
// needs root (or CAP_SYS_NICE): on a PC run as a user it quietly stays as it was.
void set_thread_nice(int nice);

}  // namespace btb
