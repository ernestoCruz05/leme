#include "timao/runtime-internal.h"
#include <errno.h>

static bool helpers(const struct timao_runtime *runtime) {
  for (size_t i = 0; i < 16; ++i)
    if (runtime->launches[i] != NULL)
      return true;
  return false;
}

int timao_runtime_shutdown(struct timao_runtime *runtime) {
  if (runtime == NULL)
    return 0;
  if (runtime->executing || runtime->writing) {
    errno = EBUSY;
    return -1;
  }
  uint64_t now = 0;
  if (!runtime->stopping) {
    runtime->stopping = true;
    timao_client_shutdown(runtime->client);
    if (timao_runtime_now(&now) != LEME_PUBLIC_OK)
      runtime->io_error = true;
    else
      runtime->shutdown_deadline =
          now > UINT64_MAX - 250 ? UINT64_MAX : now + 250;
  }
  timao_transport_step(runtime->transport, 0);
  timao_runtime_launch_tick(runtime, now);
  while (timao_transport_fd(runtime->transport) >= 0 || helpers(runtime)) {
    if (timao_runtime_now(&now) != LEME_PUBLIC_OK ||
        now >= runtime->shutdown_deadline)
      break;
    const enum timao_runtime_wait waited =
        timao_runtime_poll(runtime, runtime->shutdown_deadline);
    if (waited == TIMAO_RUNTIME_IO_ERROR || waited == TIMAO_RUNTIME_TIMEOUT)
      break;
  }
  timao_transport_destroy(runtime->transport);
  runtime->transport = NULL;
  timao_runtime_cancel_all(runtime);
  timao_runtime_launch_tick(runtime, now);
  if (helpers(runtime)) {
    errno = EBUSY;
    return -1;
  }
  return 0;
}
