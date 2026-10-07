#include "output/gpu.h"

#include "config/config.h"
#include "core/server.h"

#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define LEME_GPU_RELEASE_DELAY_MS 5000

struct leme_gpu;

struct leme_gpu_card {
  struct wl_list link;
  struct leme_gpu *gpu;
  dev_t devnum;
  char *devnode;
  bool boot;
  struct wlr_backend *backend;
  size_t output_count;
  bool keep_open;
  bool probe_pending;
  bool removed;
  struct wl_listener backend_destroy;
  struct wl_event_source *release_timer;
};

struct leme_gpu_output {
  struct wl_list link;
  struct leme_gpu_card *card;
  struct wl_listener destroy;
};

struct leme_gpu {
  struct leme_server *server;
  struct wlr_backend *primary;
  char *primary_devnode;
  struct wl_list cards;
  struct wl_list outputs;
  struct udev *udev;
  struct udev_monitor *monitor;
  struct wl_event_source *monitor_source;
  struct wl_listener session_active;
  struct wl_listener new_output;
  struct wl_listener primary_destroy;
  bool configured;
};

size_t leme_gpu_pick_primary(const struct leme_gpu_candidate *cards,
                             size_t count) {
  size_t primary = 0;

  for (size_t index = 0; index < count; index++) {
    if (cards[index].boot) {
      primary = index;
    }
  }
  return primary;
}

bool leme_gpu_is_card_name(const char *name) {
  if (name == NULL || strncmp(name, "card", 4) != 0 || name[4] == '\0') {
    return false;
  }
  for (const char *cursor = name + 4; *cursor != '\0'; cursor++) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
  }
  return true;
}

static enum leme_secondary_gpu leme_gpu_policy(const struct leme_gpu *gpu) {
  const struct leme_config *config = gpu->server->config;

  return config == NULL ? LEME_SECONDARY_GPU_ON_DEMAND
                        : config->output_policy.secondary_gpu;
}

static bool leme_gpu_udev_on_seat(struct udev_device *device,
                                  const char *seat) {
  const char *device_seat = udev_device_get_property_value(device, "ID_SEAT");

  if (device_seat == NULL) {
    device_seat = "seat0";
  }
  return strcmp(device_seat, seat) == 0;
}

static bool leme_gpu_udev_is_boot(struct udev_device *device) {
  const char *boot = udev_device_get_sysattr_value(device, "boot_display");

  if (boot != NULL && strcmp(boot, "1") == 0) {
    return true;
  }
  struct udev_device *pci =
      udev_device_get_parent_with_subsystem_devtype(device, "pci", NULL);
  if (pci == NULL) {
    return false;
  }
  const char *vga = udev_device_get_sysattr_value(pci, "boot_vga");
  return vga != NULL && strcmp(vga, "1") == 0;
}

static struct leme_gpu_card *leme_gpu_card_find(struct leme_gpu *gpu,
                                                dev_t devnum) {
  struct leme_gpu_card *card;

  wl_list_for_each(card, &gpu->cards, link) {
    if (card->devnum == devnum) {
      return card;
    }
  }
  return NULL;
}

static struct leme_gpu_card *leme_gpu_card_create(struct leme_gpu *gpu,
                                                  struct udev_device *device) {
  const char *devnode = udev_device_get_devnode(device);

  if (devnode == NULL) {
    return NULL;
  }
  struct leme_gpu_card *card = calloc(1, sizeof(*card));
  if (card == NULL) {
    return NULL;
  }
  card->devnode = strdup(devnode);
  if (card->devnode == NULL) {
    free(card);
    return NULL;
  }
  card->gpu = gpu;
  card->devnum = udev_device_get_devnum(device);
  card->boot = leme_gpu_udev_is_boot(device);
  wl_list_init(&card->backend_destroy.link);
  wl_list_insert(gpu->cards.prev, &card->link);
  return card;
}

static void leme_gpu_output_destroy(struct leme_gpu_output *tracker) {
  wl_list_remove(&tracker->destroy.link);
  wl_list_remove(&tracker->link);
  free(tracker);
}

static void leme_gpu_card_destroy(struct leme_gpu_card *card) {
  struct leme_gpu_output *tracker, *tracker_tmp;

  wl_list_for_each_safe(tracker, tracker_tmp, &card->gpu->outputs, link) {
    if (tracker->card == card) {
      leme_gpu_output_destroy(tracker);
    }
  }
  wl_list_remove(&card->backend_destroy.link);
  if (card->release_timer != NULL) {
    wl_event_source_remove(card->release_timer);
  }
  wl_list_remove(&card->link);
  free(card->devnode);
  free(card);
}

static void leme_gpu_destroy(struct leme_gpu *gpu) {
  struct leme_gpu_card *card, *card_tmp;

  if (gpu == NULL) {
    return;
  }
  wl_list_for_each_safe(card, card_tmp, &gpu->cards, link) {
    leme_gpu_card_destroy(card);
  }
  wl_list_remove(&gpu->session_active.link);
  wl_list_remove(&gpu->new_output.link);
  wl_list_remove(&gpu->primary_destroy.link);
  if (gpu->monitor_source != NULL) {
    wl_event_source_remove(gpu->monitor_source);
  }
  if (gpu->monitor != NULL) {
    udev_monitor_unref(gpu->monitor);
  }
  if (gpu->udev != NULL) {
    udev_unref(gpu->udev);
  }
  free(gpu->primary_devnode);
  free(gpu);
}

static bool leme_gpu_scan(struct leme_gpu *gpu, const char *seat) {
  struct udev_enumerate *enumerate = udev_enumerate_new(gpu->udev);
  struct udev_list_entry *entry;
  bool ok = true;

  if (enumerate == NULL) {
    return false;
  }
  if (udev_enumerate_add_match_subsystem(enumerate, "drm") < 0 ||
      udev_enumerate_add_match_sysname(enumerate, "card[0-9]*") < 0 ||
      udev_enumerate_scan_devices(enumerate) < 0) {
    udev_enumerate_unref(enumerate);
    return false;
  }
  udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
    struct udev_device *device = udev_device_new_from_syspath(
        gpu->udev, udev_list_entry_get_name(entry));

    if (device == NULL) {
      continue;
    }
    if (leme_gpu_is_card_name(udev_device_get_sysname(device)) &&
        udev_device_get_devnode(device) != NULL &&
        leme_gpu_udev_on_seat(device, seat) &&
        leme_gpu_card_create(gpu, device) == NULL) {
      ok = false;
    }
    udev_device_unref(device);
    if (!ok) {
      break;
    }
  }
  udev_enumerate_unref(enumerate);
  return ok;
}

static bool leme_gpu_choose_primary(struct leme_gpu *gpu) {
  const size_t count = (size_t)wl_list_length(&gpu->cards);
  struct leme_gpu_candidate *candidates = calloc(count, sizeof(*candidates));
  struct leme_gpu_card *card;
  size_t index = 0;

  if (candidates == NULL) {
    return false;
  }
  wl_list_for_each(card, &gpu->cards, link) {
    candidates[index] = (struct leme_gpu_candidate){
        .devnode = card->devnode,
        .boot = card->boot,
    };
    index++;
  }
  const size_t primary = leme_gpu_pick_primary(candidates, count);
  free(candidates);

  index = 0;
  wl_list_for_each(card, &gpu->cards, link) {
    if (index == primary) {
      break;
    }
    index++;
  }
  gpu->primary_devnode = card->devnode;
  card->devnode = NULL;
  leme_gpu_card_destroy(card);
  return true;
}

static struct leme_gpu *leme_gpu_create(struct leme_server *server) {
  struct leme_gpu *gpu = calloc(1, sizeof(*gpu));

  if (gpu == NULL) {
    return NULL;
  }
  gpu->server = server;
  wl_list_init(&gpu->cards);
  wl_list_init(&gpu->outputs);
  wl_list_init(&gpu->session_active.link);
  wl_list_init(&gpu->new_output.link);
  wl_list_init(&gpu->primary_destroy.link);

  const char *seat = getenv("XDG_SEAT");
  if (seat == NULL || seat[0] == '\0') {
    seat = "seat0";
  }
  gpu->udev = udev_new();
  if (gpu->udev == NULL || !leme_gpu_scan(gpu, seat) ||
      wl_list_length(&gpu->cards) < 2 || !leme_gpu_choose_primary(gpu)) {
    leme_gpu_destroy(gpu);
    return NULL;
  }
  return gpu;
}

static bool leme_gpu_fd_connected(int fd) {
  drmModeRes *resources = drmModeGetResources(fd);
  bool connected = false;

  if (resources == NULL) {
    return false;
  }
  for (int index = 0; index < resources->count_connectors && !connected;
       index++) {
    drmModeConnector *connector =
        drmModeGetConnector(fd, resources->connectors[index]);

    if (connector != NULL) {
      connected = connector->connection == DRM_MODE_CONNECTED;
      drmModeFreeConnector(connector);
    }
  }
  drmModeFreeResources(resources);
  return connected;
}

static void leme_gpu_close_device(struct wlr_session *session, dev_t devnum) {
  struct wlr_device *device;

  wl_list_for_each(device, &session->devices, link) {
    if (device->dev == devnum) {
      wlr_session_close_file(session, device);
      return;
    }
  }
}

static bool leme_gpu_session_has(struct wlr_session *session, dev_t devnum) {
  struct wlr_device *device;

  wl_list_for_each(device, &session->devices, link) {
    if (device->dev == devnum) {
      return true;
    }
  }
  return false;
}

static int leme_gpu_handle_release(void *data) {
  struct leme_gpu_card *card = data;

  if (card->backend != NULL && card->output_count == 0 && !card->keep_open &&
      leme_gpu_policy(card->gpu) == LEME_SECONDARY_GPU_ON_DEMAND) {
    wlr_log(WLR_INFO, "leme: closing %s, nothing is connected to it",
            card->devnode);
    wlr_backend_destroy(card->backend);
  }
  return 0;
}

static void leme_gpu_card_cancel_release(struct leme_gpu_card *card) {
  if (card->release_timer != NULL &&
      wl_event_source_timer_update(card->release_timer, 0) != 0) {
    wlr_log(WLR_ERROR, "leme: failed to cancel closing %s", card->devnode);
  }
}

static void leme_gpu_card_schedule_release(struct leme_gpu_card *card) {
  if (card->backend == NULL || card->output_count > 0 || card->keep_open ||
      leme_gpu_policy(card->gpu) != LEME_SECONDARY_GPU_ON_DEMAND) {
    return;
  }
  if (card->release_timer == NULL) {
    card->release_timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(card->gpu->server->display),
        leme_gpu_handle_release, card);
    if (card->release_timer == NULL) {
      wlr_log(WLR_ERROR, "leme: failed to schedule closing %s", card->devnode);
      return;
    }
  }
  if (wl_event_source_timer_update(card->release_timer,
                                   LEME_GPU_RELEASE_DELAY_MS) != 0) {
    wlr_log(WLR_ERROR, "leme: failed to schedule closing %s", card->devnode);
  }
}

static void leme_gpu_handle_backend_destroy(struct wl_listener *listener,
                                            void *data) {
  struct leme_gpu_card *card = wl_container_of(listener, card, backend_destroy);
  struct leme_gpu_output *tracker, *tracker_tmp;

  (void)data;
  wl_list_remove(&card->backend_destroy.link);
  wl_list_init(&card->backend_destroy.link);
  wl_list_for_each_safe(tracker, tracker_tmp, &card->gpu->outputs, link) {
    if (tracker->card == card) {
      leme_gpu_output_destroy(tracker);
    }
  }
  card->backend = NULL;
  card->output_count = 0;
  card->keep_open = false;
  leme_gpu_card_cancel_release(card);
  if (card->removed) {
    leme_gpu_card_destroy(card);
  }
}

static void leme_gpu_card_adopt(struct leme_gpu_card *card,
                                struct wlr_device *device) {
  struct leme_gpu *gpu = card->gpu;
  struct leme_server *server = gpu->server;
  const dev_t devnum = device->dev;
  struct wlr_backend *backend =
      wlr_drm_backend_create(server->session, device, gpu->primary);

  if (backend == NULL) {
    wlr_log(WLR_ERROR, "leme: failed to set up %s", card->devnode);
    leme_gpu_close_device(server->session, devnum);
    return;
  }
  if (!wlr_multi_backend_add(server->backend, backend)) {
    wlr_log(WLR_ERROR, "leme: failed to add %s", card->devnode);
    wlr_backend_destroy(backend);
    return;
  }
  card->backend = backend;
  card->output_count = 0;
  card->keep_open = false;
  card->backend_destroy.notify = leme_gpu_handle_backend_destroy;
  wl_list_remove(&card->backend_destroy.link);
  wl_signal_add(&backend->events.destroy, &card->backend_destroy);
  wlr_log(WLR_INFO, "leme: opened %s", card->devnode);
  if (server->started && !wlr_backend_start(backend)) {
    wlr_log(WLR_ERROR, "leme: failed to start %s", card->devnode);
    wlr_backend_destroy(backend);
    return;
  }
  leme_gpu_card_schedule_release(card);
}

static void leme_gpu_card_probe(struct leme_gpu_card *card) {
  struct leme_gpu *gpu = card->gpu;
  struct wlr_session *session = gpu->server->session;

  if (card->backend != NULL || card->removed || gpu->primary == NULL) {
    return;
  }
  if (!session->active) {
    card->probe_pending = true;
    return;
  }
  card->probe_pending = false;

  struct wlr_device *device = wlr_session_open_file(session, card->devnode);
  if (device == NULL) {
    wlr_log(WLR_ERROR, "leme: cannot open %s", card->devnode);
    return;
  }
  if (drmIsKMS(device->fd) == 0) {
    wlr_log(WLR_DEBUG, "leme: %s has no display outputs", card->devnode);
    wlr_session_close_file(session, device);
    return;
  }
  if (leme_gpu_policy(gpu) == LEME_SECONDARY_GPU_ON_DEMAND &&
      !leme_gpu_fd_connected(device->fd)) {
    wlr_log(WLR_DEBUG, "leme: nothing is connected to %s", card->devnode);
    wlr_session_close_file(session, device);
    return;
  }
  leme_gpu_card_adopt(card, device);
}

static void leme_gpu_handle_output_destroy(struct wl_listener *listener,
                                           void *data) {
  struct leme_gpu_output *tracker = wl_container_of(listener, tracker, destroy);
  struct leme_gpu_card *card = tracker->card;

  (void)data;
  leme_gpu_output_destroy(tracker);
  if (card->output_count > 0) {
    card->output_count--;
  }
  leme_gpu_card_schedule_release(card);
}

static void leme_gpu_handle_new_output(struct wl_listener *listener,
                                       void *data) {
  struct leme_gpu *gpu = wl_container_of(listener, gpu, new_output);
  struct wlr_output *output = data;
  struct leme_gpu_card *card;

  wl_list_for_each(card, &gpu->cards, link) {
    if (card->backend == NULL || card->backend != output->backend) {
      continue;
    }
    leme_gpu_card_cancel_release(card);
    struct leme_gpu_output *tracker = calloc(1, sizeof(*tracker));
    if (tracker == NULL) {
      wlr_log(WLR_ERROR, "leme: cannot track %s; keeping %s open", output->name,
              card->devnode);
      card->keep_open = true;
      return;
    }
    tracker->card = card;
    tracker->destroy.notify = leme_gpu_handle_output_destroy;
    wl_signal_add(&output->events.destroy, &tracker->destroy);
    wl_list_insert(&gpu->outputs, &tracker->link);
    card->output_count++;
    return;
  }
}

static void leme_gpu_handle_session_active(struct wl_listener *listener,
                                           void *data) {
  struct leme_gpu *gpu = wl_container_of(listener, gpu, session_active);
  struct leme_gpu_card *card, *card_tmp;

  (void)data;
  if (!gpu->server->session->active) {
    return;
  }
  wl_list_for_each_safe(card, card_tmp, &gpu->cards, link) {
    if (card->probe_pending) {
      leme_gpu_card_probe(card);
    }
  }
}

static void leme_gpu_handle_primary_destroy(struct wl_listener *listener,
                                            void *data) {
  struct leme_gpu *gpu = wl_container_of(listener, gpu, primary_destroy);

  (void)data;
  wl_list_remove(&gpu->primary_destroy.link);
  wl_list_init(&gpu->primary_destroy.link);
  gpu->primary = NULL;
}

static bool leme_gpu_udev_is_hotplug(struct udev_device *device) {
  const char *hotplug = udev_device_get_property_value(device, "HOTPLUG");

  return hotplug != NULL && strcmp(hotplug, "1") == 0;
}

static void leme_gpu_handle_udev_device(struct leme_gpu *gpu,
                                        struct udev_device *device) {
  const char *action = udev_device_get_action(device);
  const char *seat = gpu->server->session->seat;

  if (action == NULL ||
      !leme_gpu_is_card_name(udev_device_get_sysname(device)) ||
      udev_device_get_devnode(device) == NULL ||
      (seat[0] != '\0' && !leme_gpu_udev_on_seat(device, seat))) {
    return;
  }
  const dev_t devnum = udev_device_get_devnum(device);
  struct leme_gpu_card *card = leme_gpu_card_find(gpu, devnum);

  if (strcmp(action, "add") == 0) {
    if (card != NULL || leme_gpu_session_has(gpu->server->session, devnum)) {
      return;
    }
    card = leme_gpu_card_create(gpu, device);
    if (card == NULL) {
      wlr_log(WLR_ERROR, "leme: cannot track new GPU %s",
              udev_device_get_devnode(device));
      return;
    }
    wlr_log(WLR_INFO, "leme: new GPU %s", card->devnode);
    leme_gpu_card_probe(card);
  } else if (strcmp(action, "change") == 0) {
    if (card != NULL && card->backend == NULL &&
        leme_gpu_udev_is_hotplug(device)) {
      leme_gpu_card_probe(card);
    }
  } else if (strcmp(action, "remove") == 0) {
    if (card == NULL) {
      return;
    }
    if (card->backend == NULL) {
      leme_gpu_card_destroy(card);
    } else {
      card->removed = true;
    }
  }
}

static int leme_gpu_handle_udev(int fd, uint32_t mask, void *data) {
  struct leme_gpu *gpu = data;
  struct udev_device *device = udev_monitor_receive_device(gpu->monitor);

  (void)fd;
  (void)mask;
  if (device == NULL) {
    return 0;
  }
  leme_gpu_handle_udev_device(gpu, device);
  udev_device_unref(device);
  return 0;
}

static void leme_gpu_find_primary(struct wlr_backend *backend, void *data) {
  struct leme_gpu *gpu = data;

  if (gpu->primary == NULL && wlr_backend_is_drm(backend) &&
      wlr_drm_backend_get_parent(backend) == NULL) {
    gpu->primary = backend;
  }
}

static bool leme_gpu_attach(struct leme_gpu *gpu) {
  struct leme_server *server = gpu->server;
  struct leme_gpu_card *card;

  if (server->session == NULL) {
    return false;
  }
  wlr_multi_for_each_backend(server->backend, leme_gpu_find_primary, gpu);
  if (gpu->primary == NULL) {
    return false;
  }
  gpu->monitor = udev_monitor_new_from_netlink(gpu->udev, "udev");
  if (gpu->monitor == NULL ||
      udev_monitor_filter_add_match_subsystem_devtype(gpu->monitor, "drm",
                                                      NULL) < 0 ||
      udev_monitor_enable_receiving(gpu->monitor) < 0) {
    wlr_log(WLR_ERROR, "%s", "leme: cannot watch for GPU hotplug");
    return false;
  }
  gpu->monitor_source =
      wl_event_loop_add_fd(wl_display_get_event_loop(server->display),
                           udev_monitor_get_fd(gpu->monitor), WL_EVENT_READABLE,
                           leme_gpu_handle_udev, gpu);
  if (gpu->monitor_source == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: cannot watch for GPU hotplug");
    return false;
  }

  gpu->session_active.notify = leme_gpu_handle_session_active;
  wl_signal_add(&server->session->events.active, &gpu->session_active);
  gpu->new_output.notify = leme_gpu_handle_new_output;
  wl_signal_add(&server->backend->events.new_output, &gpu->new_output);
  gpu->primary_destroy.notify = leme_gpu_handle_primary_destroy;
  wl_signal_add(&gpu->primary->events.destroy, &gpu->primary_destroy);

  wl_list_for_each(card, &gpu->cards, link) {
    wlr_log(WLR_INFO, "leme: %s stays closed until a monitor is connected",
            card->devnode);
  }
  return true;
}

bool leme_gpu_create_backend(struct leme_server *server) {
  struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
  struct leme_gpu *gpu = NULL;

  if (getenv("WLR_DRM_DEVICES") == NULL) {
    gpu = leme_gpu_create(server);
  }
  if (gpu != NULL && setenv("WLR_DRM_DEVICES", gpu->primary_devnode, 1) != 0) {
    leme_gpu_destroy(gpu);
    gpu = NULL;
  }
  server->backend = wlr_backend_autocreate(loop, &server->session);
  if (gpu != NULL) {
    if (unsetenv("WLR_DRM_DEVICES") != 0) {
      wlr_log(WLR_ERROR, "%s", "leme: failed to unset WLR_DRM_DEVICES");
    }
    if (server->backend == NULL) {
      wlr_log(WLR_ERROR, "leme: cannot start on %s alone; opening every GPU",
              gpu->primary_devnode);
      leme_gpu_destroy(gpu);
      gpu = NULL;
      server->backend = wlr_backend_autocreate(loop, &server->session);
    }
  }
  if (server->backend == NULL) {
    return false;
  }
  if (gpu != NULL && !leme_gpu_attach(gpu)) {
    leme_gpu_destroy(gpu);
    gpu = NULL;
  }
  server->gpu = gpu;
  return true;
}

void leme_gpu_apply_config(struct leme_server *server) {
  struct leme_gpu *gpu = server->gpu;
  struct leme_gpu_card *card, *card_tmp;

  if (gpu == NULL) {
    return;
  }
  const bool first = !gpu->configured;
  gpu->configured = true;
  wl_list_for_each_safe(card, card_tmp, &gpu->cards, link) {
    if (card->backend == NULL) {
      if (first || leme_gpu_policy(gpu) == LEME_SECONDARY_GPU_ALWAYS) {
        leme_gpu_card_probe(card);
      }
    } else {
      leme_gpu_card_schedule_release(card);
    }
  }
}

void leme_gpu_finish(struct leme_server *server) {
  leme_gpu_destroy(server->gpu);
  server->gpu = NULL;
}
