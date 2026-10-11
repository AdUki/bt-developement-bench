#include "util/prio.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace btb {

void set_thread_nice(int nice) {
  // PRIO_PROCESS with a thread id sets that one thread, not the whole daemon.
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
}

}  // namespace btb
