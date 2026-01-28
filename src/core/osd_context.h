// OSD Context Structure
// Core data structure passed to all widgets for rendering
//
// This header is the ONLY header widgets need to access the OSD context.
// It deliberately excludes WASM-specific details to keep widget code clean.

#ifndef CORE_OSD_CONTEXT_H
#define CORE_OSD_CONTEXT_H

#include "../config/osd_config.h"
#include "../resources/font.h"
#include "../resources/svg.h"
#include "framebuffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ════════════════════════════════════════════════════════════
// OSD CONTEXT
// ════════════════════════════════════════════════════════════
//
// The osd_context_t structure contains everything a widget needs:
//   - Framebuffer to render into
//   - Configuration (colors, positions, sizes)
//   - Pre-loaded resources (fonts, SVGs)
//   - Render state (frame count)
//
// Widgets should NOT modify context fields directly except through
// the provided helper functions.

typedef struct osd_context
{
  // ──────────────────────────────────────────────────────────
  // FRAMEBUFFER (render target)
  // ──────────────────────────────────────────────────────────
  uint32_t *framebuffer;
  uint32_t width;
  uint32_t height;

  // ──────────────────────────────────────────────────────────
  // CONFIGURATION (loaded from JSON at init)
  // ──────────────────────────────────────────────────────────
  osd_config_t config;

  // ──────────────────────────────────────────────────────────
  // RESOURCES (pre-loaded at init)
  // ──────────────────────────────────────────────────────────
  // Per-widget fonts (each widget can have its own font)
  font_resource_t font_timestamp;
  font_resource_t font_speed_indicators;
  font_resource_t font_variant_info;

  svg_resource_t cross_svg;  // Crosshair SVG icon
  svg_resource_t circle_svg; // Circle SVG icon

  // ──────────────────────────────────────────────────────────
  // INTERNAL STATE (managed by framework - widgets read-only)
  // ──────────────────────────────────────────────────────────

  // Proto buffer (internal - use osd_state.h accessors instead)
  uint8_t proto_buffer[4096];
  size_t proto_size;
  bool proto_valid;

  // Client metadata from opaque payload (canvas info from frontend)
  struct
  {
    uint32_t canvas_width_px;
    uint32_t canvas_height_px;
    float device_pixel_ratio;
    uint32_t osd_buffer_width;
    uint32_t osd_buffer_height;
    // Video proxy bounds (NDC -1.0 to 1.0)
    float video_proxy_ndc_x;
    float video_proxy_ndc_y;
    float video_proxy_ndc_width;
    float video_proxy_ndc_height;
    // Scale factor: osd_buffer_pixels / proxy_physical_pixels
    float scale_factor;
    // Theme info
    bool is_sharp_mode;
    float theme_hue;
    float theme_chroma;
    float theme_lightness;
    bool valid;
  } client_metadata;

  // Nav ball state
  bool navball_enabled;
  int navball_x;
  int navball_y;
  int navball_size;
  navball_skin_t navball_skin;
  bool navball_show_level_marker;
  void *navball_texture; // Pointer to texture_t (opaque)
  void *navball_lut;     // Pointer to navball_lut_t (opaque)

  // Nav ball center indicator
  bool navball_show_center_indicator;
  float navball_center_indicator_scale;
  svg_resource_t navball_center_indicator_svg;

  // Celestial indicators (sun and moon on navball)
  bool celestial_enabled;
  bool celestial_show_sun;
  bool celestial_show_moon;
  float celestial_indicator_scale;
  float celestial_visibility_threshold;
  svg_resource_t celestial_sun_front_svg;
  svg_resource_t celestial_sun_back_svg;
  svg_resource_t celestial_moon_front_svg;
  svg_resource_t celestial_moon_back_svg;

  // Rendering state
  bool needs_render;
  uint32_t frame_count;
} osd_context_t;

// ════════════════════════════════════════════════════════════
// CONTEXT HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════

// Convert OSD context to framebuffer view (for rendering primitives)
//
// Usage:
//   framebuffer_t fb = osd_ctx_get_framebuffer(ctx);
//   draw_line(&fb, x0, y0, x1, y1, color, thickness);
static inline framebuffer_t
osd_ctx_get_framebuffer(osd_context_t *ctx)
{
  framebuffer_t fb;
  framebuffer_init(&fb, ctx->framebuffer, ctx->width, ctx->height);
  return fb;
}

// Get screen center coordinates
static inline void
osd_ctx_get_center(const osd_context_t *ctx, int *cx, int *cy)
{
  *cx = (int)ctx->width / 2;
  *cy = (int)ctx->height / 2;
}

#endif // CORE_OSD_CONTEXT_H
