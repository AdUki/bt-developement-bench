#include "util/prio.h"

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace btb {

void set_thread_nice(int nice) {
  // PRIO_PROCESS with a thread id sets that one thread, not the whole daemon.
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
}

void set_process_fifo(int pid, int prio) {
  sched_param sp{};
  sp.sched_priority = prio;
  (void)sched_setscheduler(pid, SCHED_FIFO, &sp);
}

}  // namespace btb
