#include "desktop/pixels.h"

#include <cairo.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

struct leme_desk_buffer {
  struct wlr_buffer *buffer;
  struct wl_listener destroy;
};

static bool leme_desk_reject_input(struct wlr_scene_buffer *buffer, double *sx,
                                   double *sy) {
  (void)buffer;
  (void)sx;
  (void)sy;
  return false;
}

static void leme_desk_buffer_handle_destroy(struct wl_listener *listener,
                                            void *data) {
  struct leme_desk_buffer *reference =
      wl_container_of(listener, reference, destroy);
  (void)data;
  wl_list_remove(&reference->destroy.link);
  wlr_buffer_unlock(reference->buffer);
  free(reference);
}

bool leme_desk_pixels_place_buffer(struct wlr_scene_tree *tree,
                                   struct wlr_buffer *buffer,
                                   struct leme_box box) {
  if (tree == NULL || buffer == NULL || box.width <= 0 || box.height <= 0) {
    return false;
  }
  struct leme_desk_buffer *reference = calloc(1, sizeof(*reference));
  if (reference == NULL) {
    return false;
  }
  struct wlr_scene_buffer *node = wlr_scene_buffer_create(tree, buffer);
  if (node == NULL) {
    free(reference);
    return false;
  }
  reference->buffer = wlr_buffer_lock(buffer);
  reference->destroy.notify = leme_desk_buffer_handle_destroy;
  wl_signal_add(&node->node.events.destroy, &reference->destroy);
  node->point_accepts_input = leme_desk_reject_input;
  wlr_scene_buffer_set_dest_size(node, box.width, box.height);
  wlr_scene_node_set_position(&node->node, box.x, box.y);
  return true;
}

bool leme_desk_pixels_rect(struct wlr_scene_tree *tree, struct leme_box box,
                           const float color[4], int radius, bool ring) {
#ifdef LEME_HAVE_EFFECTS
  struct wlr_scene_rect *rect =
      wlr_scene_rect_create(tree, box.width, box.height, color);
  if (rect == NULL) {
    return false;
  }
  wlr_scene_node_set_position(&rect->node, box.x, box.y);
  wlr_scene_rect_set_corner_radius(rect, (float)radius, ring ? 1.0f : 0.0f);
#else
  (void)radius;
  const struct leme_box edges[] = {
      {box.x, box.y, box.width, 1},
      {box.x, box.y + box.height - 1, box.width, 1},
      {box.x, box.y + 1, 1, box.height - 2},
      {box.x + box.width - 1, box.y + 1, 1, box.height - 2}};
  for (size_t i = 0; i < (ring ? LEME_ARRAY_LENGTH(edges) : 1); i++) {
    const struct leme_box edge = ring ? edges[i] : box;
    struct wlr_scene_rect *rect =
        wlr_scene_rect_create(tree, edge.width, edge.height, color);
    if (rect == NULL) {
      return false;
    }
    wlr_scene_node_set_position(&rect->node, edge.x, edge.y);
  }
#endif
  return true;
}

struct leme_desk_pixels {
  struct wlr_buffer base;
  cairo_surface_t *surface;
};

struct leme_desk_bytes {
  unsigned char *data;
  size_t length;
  size_t offset;
};

static void leme_desk_pixels_destroy(struct wlr_buffer *base) {
  struct leme_desk_pixels *pixels = wl_container_of(base, pixels, base);
  wlr_buffer_finish(base);
  cairo_surface_destroy(pixels->surface);
  free(pixels);
}

static bool leme_desk_pixels_begin(struct wlr_buffer *base, uint32_t flags,
                                   void **data, uint32_t *format,
                                   size_t *stride) {
  struct leme_desk_pixels *pixels = wl_container_of(base, pixels, base);
  if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
    return false;
  }
  *data = cairo_image_surface_get_data(pixels->surface);
  *stride = (size_t)cairo_image_surface_get_stride(pixels->surface);
  *format = DRM_FORMAT_ARGB8888;
#if G_BYTE_ORDER == G_BIG_ENDIAN
  *format |= DRM_FORMAT_BIG_ENDIAN;
#endif
  return true;
}

static void leme_desk_pixels_end(struct wlr_buffer *base) { (void)base; }

static const struct wlr_buffer_impl leme_desk_pixels_impl = {
    .destroy = leme_desk_pixels_destroy,
    .begin_data_ptr_access = leme_desk_pixels_begin,
    .end_data_ptr_access = leme_desk_pixels_end,
};

static struct wlr_buffer *leme_desk_pixels_wrap(cairo_surface_t *surface) {
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
      cairo_image_surface_get_stride(surface) <= 0) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  struct leme_desk_pixels *pixels = calloc(1, sizeof(*pixels));
  if (pixels == NULL) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  cairo_surface_flush(surface);
  pixels->surface = surface;
  wlr_buffer_init(&pixels->base, &leme_desk_pixels_impl,
                  cairo_image_surface_get_width(surface),
                  cairo_image_surface_get_height(surface));
  return &pixels->base;
}

static bool leme_desk_bytes_read(const char *path,
                                 struct leme_desk_bytes *bytes) {
  const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  bool ok = false;
  struct stat status = {0};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || status.st_size > 16 * 1024 * 1024) {
    goto cleanup;
  }
  const size_t size = (size_t)status.st_size;
  bytes->data = malloc(size);
  if (bytes->data == NULL) {
    goto cleanup;
  }
  while (bytes->length < size) {
    const ssize_t count =
        read(fd, bytes->data + bytes->length, size - bytes->length);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      goto cleanup;
    }
    if (count == 0) {
      break;
    }
    bytes->length += (size_t)count;
  }
  ok = bytes->length > 0;
cleanup:
  if (close(fd) != 0) {
    ok = false;
  }
  if (!ok) {
    free(bytes->data);
    *bytes = (struct leme_desk_bytes){0};
  }
  return ok;
}

static bool leme_desk_pixels_svg(cairo_t *cr, const char *path,
                                 const struct leme_desk_bytes *bytes,
                                 int size) {
  GInputStream *stream = g_memory_input_stream_new_from_data(
      bytes->data, (gssize)bytes->length, NULL);
  GFile *base = g_file_new_for_path(path);
  if (stream == NULL || base == NULL) {
    g_clear_object(&stream);
    g_clear_object(&base);
    return false;
  }
  GError *error = NULL;
  RsvgHandle *handle = rsvg_handle_new_from_stream_sync(
      stream, base, RSVG_HANDLE_FLAGS_NONE, NULL, &error);
  bool ok = false;
  if (handle != NULL) {
    const RsvgRectangle viewport = {.width = size, .height = size};
    ok = rsvg_handle_render_document(handle, cr, &viewport, &error);
    g_object_unref(handle);
  }
  g_clear_error(&error);
  g_object_unref(base);
  g_object_unref(stream);
  return ok;
}

static cairo_status_t leme_desk_pixels_png_read(void *data, unsigned char *out,
                                                unsigned int length) {
  struct leme_desk_bytes *bytes = data;
  if (length > bytes->length - bytes->offset) {
    return CAIRO_STATUS_READ_ERROR;
  }
  memcpy(out, bytes->data + bytes->offset, length);
  bytes->offset += length;
  return CAIRO_STATUS_SUCCESS;
}

static uint32_t leme_desk_pixels_png_dimension(const unsigned char *data) {
  uint32_t result = 0;
  for (size_t i = 0; i < 4; i++) {
    result = (result << 8) | data[i];
  }
  return result;
}

static bool leme_desk_pixels_png(cairo_t *cr, struct leme_desk_bytes *bytes,
                                 int size) {
  if (bytes->length < 24 || memcmp(bytes->data, "\x89PNG\r\n\x1a\n", 8) != 0 ||
      memcmp(bytes->data + 12, "IHDR", 4) != 0) {
    return false;
  }
  const uint32_t width = leme_desk_pixels_png_dimension(bytes->data + 16);
  const uint32_t height = leme_desk_pixels_png_dimension(bytes->data + 20);
  if (width == 0 || height == 0 || width > 4096 || height > 4096) {
    return false;
  }
  cairo_surface_t *source = cairo_image_surface_create_from_png_stream(
      leme_desk_pixels_png_read, bytes);
  if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(source);
    return false;
  }
  const double scale = (double)size / (width > height ? width : height);
  cairo_translate(cr, ((double)size - width * scale) / 2.0,
                  ((double)size - height * scale) / 2.0);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint(cr);
  cairo_surface_destroy(source);
  return true;
}

struct wlr_buffer *leme_desk_pixels_load(const char *path, int size) {
  if (path == NULL || size < 1 || size > 4096) {
    return NULL;
  }
  const char *extension = strrchr(path, '.');
  if (extension == NULL) {
    return NULL;
  }
  const bool svg = g_ascii_strcasecmp(extension, ".svg") == 0;
  if (!svg && g_ascii_strcasecmp(extension, ".png") != 0) {
    return NULL;
  }
  struct leme_desk_bytes bytes = {0};
  if (!leme_desk_bytes_read(path, &bytes)) {
    return NULL;
  }
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surface);
  bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  if (ok) {
    ok = svg ? leme_desk_pixels_svg(cr, path, &bytes, size)
             : leme_desk_pixels_png(cr, &bytes, size);
  }
  ok = ok && cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  cairo_destroy(cr);
  free(bytes.data);
  if (!ok) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  return leme_desk_pixels_wrap(surface);
}

void leme_desk_utf8_copy(char *out, size_t capacity, const char *text) {
  if (out == NULL || capacity == 0) {
    return;
  }
  const size_t length = text == NULL ? 0 : strnlen(text, capacity - 1);
  if (length > 0) {
    memcpy(out, text, length);
  }
  out[length] = '\0';
  const char *end = NULL;
  if (!g_utf8_validate(out, -1, &end)) {
    out[end - out] = '\0';
  }
}

struct leme_desk_text_style {
  const char *font;
  const float *color;
  bool shadow;
  bool single_line;
  bool entry;
  size_t cursor;
};

static void leme_desk_pixels_source(cairo_t *cr, const float color[4]) {
  const double alpha = (double)color[3];
  if (alpha <= 0.0) {
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    return;
  }
  cairo_set_source_rgba(cr, (double)color[0] / alpha, (double)color[1] / alpha,
                        (double)color[2] / alpha, alpha);
}

static PangoLayout *leme_desk_pixels_layout(cairo_t *cr,
                                            const PangoFontDescription *font,
                                            const char *text, int width,
                                            bool single_line) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  if (layout == NULL) {
    return NULL;
  }
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), 96.0);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  pango_layout_set_height(layout, single_line ? -1 : -2);
  pango_layout_set_single_paragraph_mode(layout, single_line);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(layout, single_line ? PANGO_ALIGN_LEFT
                                                 : PANGO_ALIGN_CENTER);
  pango_layout_set_text(layout, text, -1);
  return layout;
}

static struct wlr_buffer *leme_desk_pixels_text(
    const char *text, const struct leme_desk_text_style *style,
    int logical_width, double scale, int *fitted_width, int *logical_height) {
  if (logical_height == NULL) {
    return NULL;
  }
  *logical_height = 0;
  if (text == NULL || !g_utf8_validate(text, -1, NULL) || logical_width < 1 ||
      logical_width > 1024 || !isfinite(scale) || scale < 0.5 || scale > 4.0) {
    return NULL;
  }
  if (style->entry &&
      (strnlen(text, 1025) > 1024 ||
       (style->cursor != SIZE_MAX &&
        (style->cursor > strlen(text) ||
         !g_utf8_validate(text, (gssize)style->cursor, NULL))))) {
    return NULL;
  }
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(surface);
  if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return NULL;
  }
  cairo_scale(cr, scale, scale);
  PangoFontDescription *font = pango_font_description_from_string(style->font);
  PangoLayout *layout = NULL;
  int height = 0;
  bool ok = false;
  if (font == NULL) {
    goto cleanup;
  }
  layout = leme_desk_pixels_layout(cr, font, text, logical_width - 1,
                                   style->single_line);
  if (layout == NULL) {
    goto cleanup;
  }
  PangoRectangle caret = {0};
  double scroll = 0.0;
  if (style->entry) {
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    const size_t cursor =
        style->cursor == SIZE_MAX ? strlen(text) : style->cursor;
    pango_layout_get_cursor_pos(layout, (int)cursor, &caret, NULL);
    scroll = fmax(0.0, (double)caret.x / PANGO_SCALE - logical_width + 2.0);
  }
  PangoRectangle ink = {0};
  PangoRectangle logical = {0};
  pango_layout_get_pixel_extents(layout, &ink, &logical);
  const double top = fmin(0.0, ink.y);
  const double bottom =
      fmax((double)ink.y + ink.height, (double)logical.y + logical.height);
  const double measured_height = bottom - top + 1.0;
  if (measured_height < 1.0 || measured_height > 4096.0 / scale) {
    goto cleanup;
  }
  height = (int)measured_height;
  int fitted = logical_width;
  if (fitted_width != NULL) {
    const int extent = logical.x + logical.width + 1;
    fitted = extent < 1 ? 1 : (extent < logical_width ? extent : logical_width);
  }
  const int width = (int)ceil(fitted * scale);
  const int pixels = (int)ceil(measured_height * scale);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, pixels);
  cr = cairo_create(surface);
  if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
    goto cleanup;
  }
  cairo_scale(cr, scale, scale);
  pango_cairo_update_layout(cr, layout);
  if (style->shadow) {
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
    cairo_move_to(cr, 1.0, 1.0 - top);
    pango_cairo_show_layout(cr, layout);
  }
  leme_desk_pixels_source(cr, style->color);
  cairo_move_to(cr, -scroll, -top);
  pango_cairo_show_layout(cr, layout);
  if (style->entry && style->cursor != SIZE_MAX) {
    cairo_rectangle(cr, (double)caret.x / PANGO_SCALE - scroll,
                    (double)caret.y / PANGO_SCALE - top, 1.0,
                    (double)caret.height / PANGO_SCALE);
    cairo_fill(cr);
  }
  ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
cleanup:
  if (font != NULL) {
    pango_font_description_free(font);
  }
  g_clear_object(&layout);
  cairo_destroy(cr);
  if (!ok) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  struct wlr_buffer *buffer = leme_desk_pixels_wrap(surface);
  if (buffer != NULL) {
    *logical_height = height;
    if (fitted_width != NULL) {
      *fitted_width = fitted;
    }
  }
  return buffer;
}

struct wlr_buffer *leme_desk_pixels_label(const char *text, const char *font,
                                          const float color[4], bool shadow,
                                          int logical_width, double scale,
                                          int *logical_height) {
  if (font == NULL || color == NULL) {
    return NULL;
  }
  const struct leme_desk_text_style style = {
      .font = font, .color = color, .shadow = shadow, .single_line = false};
  return leme_desk_pixels_text(text, &style, logical_width, scale, NULL,
                               logical_height);
}

struct wlr_buffer *leme_desk_pixels_line(const char *text, const char *font,
                                         const float color[4],
                                         int logical_width, double scale,
                                         int *fitted_width,
                                         int *logical_height) {
  if (font == NULL || color == NULL || fitted_width == NULL) {
    return NULL;
  }
  const struct leme_desk_text_style style = {
      .font = font, .color = color, .shadow = false, .single_line = true};
  return leme_desk_pixels_text(text, &style, logical_width, scale, fitted_width,
                               logical_height);
}

struct wlr_buffer *leme_desk_pixels_entry(const char *text, const char *font,
                                          const float color[4],
                                          int logical_width, double scale,
                                          size_t cursor, int *logical_height) {
  if (font == NULL || color == NULL) {
    return NULL;
  }
  const struct leme_desk_text_style style = {.font = font,
                                             .color = color,
                                             .single_line = true,
                                             .entry = true,
                                             .cursor = cursor};
  return leme_desk_pixels_text(text, &style, logical_width, scale, NULL,
                               logical_height);
}

bool leme_desk_pixels_place_line(struct wlr_scene_tree *tree, const char *text,
                                 const char *font, const float color[4],
                                 double scale, struct leme_box box,
                                 bool center) {
  int width = 0;
  int height = 0;
  struct wlr_buffer *buffer = leme_desk_pixels_line(
      text, font, color, box.width, scale, &width, &height);
  if (buffer == NULL) {
    return false;
  }
  height = height < box.height ? height : box.height;
  const bool ok = leme_desk_pixels_place_buffer(
      tree, buffer,
      (struct leme_box){box.x + (center ? (box.width - width) / 2 : 0),
                        box.y + (box.height - height) / 2, width, height});
  wlr_buffer_drop(buffer);
  return ok;
}

struct wlr_buffer *leme_desk_pixels_control(enum leme_desk_control_icon icon,
                                            const float color[4], int size,
                                            double scale) {
  if (color == NULL || size < 1 || size > 256 || !isfinite(scale) ||
      scale < 0.5 || scale > 4.0) {
    return NULL;
  }
  const int pixels = (int)ceil(size * scale);
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
  cairo_t *cr = cairo_create(surface);
  cairo_scale(cr, scale * size / 32.0, scale * size / 32.0);
  leme_desk_pixels_source(cr, color);
  cairo_set_line_width(cr, 1.5);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  switch (icon) {
  case LEME_DESK_CONTROL_MINIMIZE:
    cairo_move_to(cr, 11.5, 14.0);
    cairo_line_to(cr, 16.0, 18.5);
    cairo_line_to(cr, 20.5, 14.0);
    break;
  case LEME_DESK_CONTROL_MAXIMIZE:
    cairo_move_to(cr, 11.5, 18.0);
    cairo_line_to(cr, 16.0, 13.5);
    cairo_line_to(cr, 20.5, 18.0);
    break;
  case LEME_DESK_CONTROL_RESTORE:
    cairo_move_to(cr, 16.0, 11.5);
    cairo_line_to(cr, 20.5, 16.0);
    cairo_line_to(cr, 16.0, 20.5);
    cairo_line_to(cr, 11.5, 16.0);
    cairo_close_path(cr);
    break;
  case LEME_DESK_CONTROL_CLOSE:
    cairo_move_to(cr, 12.5, 12.5);
    cairo_line_to(cr, 19.5, 19.5);
    cairo_move_to(cr, 19.5, 12.5);
    cairo_line_to(cr, 12.5, 19.5);
    break;
  default:
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return NULL;
  }
  cairo_stroke(cr);
  const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  cairo_destroy(cr);
  if (!ok) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  return leme_desk_pixels_wrap(surface);
}
