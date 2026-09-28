#include "render/timing.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define LEME_RENDER_TIMING_MAX_SECONDS 60
#define LEME_RENDER_TIMING_SAMPLES_PER_SECOND 512

struct leme_render_timing {
  struct wlr_scene_timer timer;
  bool pending;
  bool window_started;
  uint32_t interval_seconds;
  struct timespec window_start;
  size_t frames;
  int64_t cpu_total_ns;
  int64_t cpu_max_ns;
  size_t gpu_count;
  int64_t gpu_total_ns;
  int64_t gpu_max_ns;
  size_t sample_count;
  size_t sample_capacity;
  int64_t *samples;
};

bool leme_render_timing_parse(const char *value, uint32_t *seconds) {
  char *end = NULL;
  long parsed;

  *seconds = 0;
  if (value == NULL || value[0] == '\0') {
    return true;
  }
  errno = 0;
  parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < 1 ||
      parsed > LEME_RENDER_TIMING_MAX_SECONDS) {
    return false;
  }
  *seconds = (uint32_t)parsed;
  return true;
}

struct leme_render_timing *
leme_render_timing_create(uint32_t interval_seconds) {
  struct leme_render_timing *timing;

  if (interval_seconds == 0 ||
      interval_seconds > LEME_RENDER_TIMING_MAX_SECONDS) {
    return NULL;
  }
  timing = calloc(1, sizeof(*timing));
  if (timing == NULL) {
    return NULL;
  }
  timing->interval_seconds = interval_seconds;
  timing->sample_capacity =
      (size_t)interval_seconds * LEME_RENDER_TIMING_SAMPLES_PER_SECOND;
  timing->samples = calloc(timing->sample_capacity, sizeof(*timing->samples));
  if (timing->samples == NULL) {
    free(timing);
    return NULL;
  }
  return timing;
}

void leme_render_timing_destroy(struct leme_render_timing *timing) {
  if (timing == NULL) {
    return;
  }
  wlr_scene_timer_finish(&timing->timer);
  free(timing->samples);
  free(timing);
}

static int64_t leme_render_timing_nsec(const struct timespec *time) {
  return (int64_t)time->tv_sec * 1000000000 + (int64_t)time->tv_nsec;
}

static double leme_render_timing_msec(int64_t nsec) {
  return (double)nsec / 1e6;
}

static int leme_render_timing_compare(const void *first, const void *second) {
  const int64_t left = *(const int64_t *)first;
  const int64_t right = *(const int64_t *)second;

  return (left > right) - (left < right);
}

static void leme_render_timing_report(struct leme_render_timing *timing,
                                      const char *name, int64_t elapsed_ns) {
  const double seconds = (double)elapsed_ns / 1e9;
  const double cpu_avg =
      leme_render_timing_msec(timing->cpu_total_ns) / (double)timing->frames;
  const double cpu_max = leme_render_timing_msec(timing->cpu_max_ns);

  if (timing->gpu_count == 0 || timing->sample_count == 0) {
    wlr_log(WLR_INFO,
            "leme: render timing %s: %zu frames in %.1f s; cpu avg %.3f ms "
            "max %.3f ms; gpu n/a",
            name, timing->frames, seconds, cpu_avg, cpu_max);
    return;
  }
  qsort(timing->samples, timing->sample_count, sizeof(*timing->samples),
        leme_render_timing_compare);
  const size_t p95_rank = (timing->sample_count * 95 + 99) / 100;
  wlr_log(WLR_INFO,
          "leme: render timing %s: %zu frames in %.1f s; cpu avg %.3f ms "
          "max %.3f ms; gpu avg %.3f ms p95 %.3f ms max %.3f ms (%zu timed)",
          name, timing->frames, seconds, cpu_avg, cpu_max,
          leme_render_timing_msec(timing->gpu_total_ns) /
              (double)timing->gpu_count,
          leme_render_timing_msec(timing->samples[p95_rank - 1]),
          leme_render_timing_msec(timing->gpu_max_ns), timing->gpu_count);
}

static void leme_render_timing_reset(struct leme_render_timing *timing,
                                     const struct timespec *now) {
  timing->window_start = *now;
  timing->frames = 0;
  timing->cpu_total_ns = 0;
  timing->cpu_max_ns = 0;
  timing->gpu_count = 0;
  timing->gpu_total_ns = 0;
  timing->gpu_max_ns = 0;
  timing->sample_count = 0;
}

static void leme_render_timing_collect_gpu(struct leme_render_timing *timing) {
  int duration;

  if (!timing->pending || timing->timer.render_timer == NULL) {
    return;
  }
  duration = wlr_render_timer_get_duration_ns(timing->timer.render_timer);
  if (duration < 0) {
    return;
  }
  timing->gpu_count++;
  timing->gpu_total_ns += duration;
  if (duration > timing->gpu_max_ns) {
    timing->gpu_max_ns = duration;
  }
  if (timing->sample_count < timing->sample_capacity) {
    timing->samples[timing->sample_count++] = duration;
  }
}

struct wlr_scene_timer *
leme_render_timing_begin(struct leme_render_timing *timing, const char *name) {
  struct timespec now;

  if (timing == NULL) {
    return NULL;
  }
  leme_render_timing_collect_gpu(timing);
  timing->pending = false;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!timing->window_started) {
    timing->window_started = true;
    leme_render_timing_reset(timing, &now);
    return &timing->timer;
  }
  const int64_t elapsed_ns = leme_render_timing_nsec(&now) -
                             leme_render_timing_nsec(&timing->window_start);
  if (elapsed_ns >= (int64_t)timing->interval_seconds * 1000000000) {
    if (timing->frames > 0) {
      leme_render_timing_report(timing, name, elapsed_ns);
    }
    leme_render_timing_reset(timing, &now);
  }
  return &timing->timer;
}

void leme_render_timing_end(struct leme_render_timing *timing,
                            const struct timespec *start, bool rendered) {
  struct timespec now;
  int64_t cpu_ns;

  if (timing == NULL || !rendered) {
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &now);
  cpu_ns = leme_render_timing_nsec(&now) - leme_render_timing_nsec(start);
  timing->frames++;
  timing->cpu_total_ns += cpu_ns;
  if (cpu_ns > timing->cpu_max_ns) {
    timing->cpu_max_ns = cpu_ns;
  }
  timing->pending = true;
}
