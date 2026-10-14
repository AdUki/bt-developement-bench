#pragma once

namespace btb {

// CPU priorities on a board with one core. Audio comes first: a stream's pump and the decoder and
// pw-cat it starts sit above WirePlumber's main loop (nice -11), whose Lua rescans after a volume
// change take a Zero W a quarter of a second — long enough to starve a nice-0 pw-cat into xruns.
// What only records or shows (graphs, the D-Bus log, the HCI monitor, pw-top, the journal view)
// sits below everything else: it can fall a second behind, the audio cannot.
constexpr int kAudioNice = -15;
constexpr int kBackgroundNice = 10;

// What a stream records from or plays into (pw-cat; arecord, aplay without PipeWire) runs
// SCHED_FIFO at this priority, its decoder not. pw-cat moves
// each 512-frame cycle (10.7 ms) in its main loop, with a handful of buffers in between: a nice
// value alone left it late often enough on a Zero W that PipeWire dropped a cycle about once a
// second (a click, and a listener's buffer draining), FIFO none. Under PipeWire's data loops (40
// on the Zero W, 88 elsewhere) and the kernel's interrupt threads (50).
constexpr int kAudioRtPrio = 30;

// The calling thread's nice value (Linux keeps one per thread). Threads and processes it starts
// afterwards inherit it, which is how a pump's decoder and sink get theirs. Raising a priority
// needs root (or CAP_SYS_NICE): on a PC run as a user it quietly stays as it was.
void set_thread_nice(int nice);

// Process `pid` (its main thread, and the threads it starts from then on) to SCHED_FIFO `prio`.
// Needs root or CAP_SYS_NICE as well: without, it stays as it was.
void set_process_fifo(int pid, int prio);

}  // namespace btb
