#include "widgets/crosshair.h"

#include "core/context_helpers.h"
#include "jon_shared_data.pb.h"
#include "rendering/primitives.h"
#include "rendering/text.h"

#include <math.h>
#include <stdio.h>

// Define fabs builtin for WASI SDK
#ifndef fabs
#define fabs(x) __builtin_fabs(x)
#endif

// Speed indicator positioning (radial around crosshair, like web version)
// Asymmetric radii: horizontal (110px) > vertical (90px) for optimal layout
// Vertical increased from 55px to 90px for better spacing with larger font
#define SPEED_RADIUS_HORIZONTAL \
  110 // Distance from center for Az speed (left/right)
#define SPEED_RADIUS_VERTICAL \
  90 // Distance from center for El speed (top/bottom)

// Speed indicator text clearing dimensions
#define SPEED_INDICATOR_CLEAR_WIDTH 100 // Estimated max text width for clearing
#define SPEED_INDICATOR_CLEAR_HEIGHT_PADDING 10 // Extra padding for text height

// Speed indicator bounding box calculations
#define SPEED_INDICATOR_TEXT_WIDTH_HALF \
  50 // Half of estimated text width for bbox
#define SPEED_INDICATOR_TEXT_HEIGHT_PADDING \
  5 // Extra padding for text height in bbox

// ════════════════════════════════════════════════════════════
// CROSSHAIR ELEMENT RENDERING
// ════════════════════════════════════════════════════════════

void
crosshair_render_center_dot(framebuffer_t *fb,
                            const crosshair_config_t *config,
                            int cx,
                            int cy)
{
  if (!config->center_dot.enabled)
    return;

  draw_filled_circle(fb, cx, cy, config->center_dot_radius,
                     config->center_dot.color);
}

void
crosshair_render_cross(framebuffer_t *fb,
                       const crosshair_config_t *config,
                       int cx,
                       int cy)
{
  if (!config->cross.enabled)
    return;

  float gap       = config->cross_gap;
  float length    = config->cross_length;
  uint32_t color  = config->cross.color;
  float thickness = config->cross.thickness;

  if (config->orientation == CROSSHAIR_ORIENTATION_VERTICAL)
    {
      // Vertical orientation (+ shape)
      // Top line
      draw_line(fb, cx, cy - gap, cx, cy - gap - length, color, thickness);
      // Bottom line
      draw_line(fb, cx, cy + gap, cx, cy + gap + length, color, thickness);
      // Left line
      draw_line(fb, cx - gap, cy, cx - gap - length, cy, color, thickness);
      // Right line
      draw_line(fb, cx + gap, cy, cx + gap + length, cy, color, thickness);
    }
  else
    {
      // Diagonal orientation (X shape - 45 degrees)
      float diag_gap = gap * 0.707f; // cos(45°) = sin(45°) = √2/2
      float diag_len = length * 0.707f;

      // Top-right line
      draw_line(fb, cx + diag_gap, cy - diag_gap, cx + diag_gap + diag_len,
                cy - diag_gap - diag_len, color, thickness);
      // Bottom-right line
      draw_line(fb, cx + diag_gap, cy + diag_gap, cx + diag_gap + diag_len,
                cy + diag_gap + diag_len, color, thickness);
      // Bottom-left line
      draw_line(fb, cx - diag_gap, cy + diag_gap, cx - diag_gap - diag_len,
                cy + diag_gap + diag_len, color, thickness);
      // Top-left line
      draw_line(fb, cx - diag_gap, cy - diag_gap, cx - diag_gap - diag_len,
                cy - diag_gap - diag_len, color, thickness);
    }
}

void
crosshair_render_circle(framebuffer_t *fb,
                        const crosshair_config_t *config,
                        int cx,
                        int cy)
{
  if (!config->circle.enabled)
    return;

  draw_circle_outline(fb, cx, cy, config->circle_radius, config->circle.color,
                      config->circle.thickness);
}

// ════════════════════════════════════════════════════════════
// SPEED INDICATOR RENDERING (radial around crosshair)
// ════════════════════════════════════════════════════════════

// Clear all 4 speed indicator positions when rotary stops moving
// Matches web version: wasm_osd_day_lib.c:245-270
static void
clear_speed_indicators(framebuffer_t *fb, int cx, int cy, int font_size)
{
  // Estimate maximum text width/height for clearing
  // Typical: "123.5°/s" = ~80px width, ~24px height at 14px font
  int clear_width  = SPEED_INDICATOR_CLEAR_WIDTH;
  int clear_height = font_size + SPEED_INDICATOR_CLEAR_HEIGHT_PADDING;

  // Clear LEFT position (azimuth negative)
  int left_x = cx - SPEED_RADIUS_HORIZONTAL - (clear_width / 2);
  int left_y = cy - (clear_height / 2);
  draw_rect_filled(fb, left_x, left_y, clear_width, clear_height, 0x00000000);

  // Clear RIGHT position (azimuth positive)
  int right_x = cx + SPEED_RADIUS_HORIZONTAL - (clear_width / 2);
  int right_y = cy - (clear_height / 2);
  draw_rect_filled(fb, right_x, right_y, clear_width, clear_height, 0x00000000);

  // Clear TOP position (elevation positive)
  int top_x = cx - (clear_width / 2);
  int top_y = cy - SPEED_RADIUS_VERTICAL - (clear_height / 2);
  draw_rect_filled(fb, top_x, top_y, clear_width, clear_height, 0x00000000);

  // Clear BOTTOM position (elevation negative)
  int bottom_x = cx - (clear_width / 2);
  int bottom_y = cy + SPEED_RADIUS_VERTICAL - (clear_height / 2);
  draw_rect_filled(fb, bottom_x, bottom_y, clear_width, clear_height,
                   0x00000000);
}

// Render azimuth speed indicator (left/right of crosshair)
// Matches web version: wasm_osd_day_lib.c:171-197
static void
render_azimuth_speed(framebuffer_t *fb,
                     const font_resource_t *font,
                     int cx,
                     int cy,
                     double speed,
                     uint32_t color,
                     int font_size)
{
  char text[32];
  snprintf(text, sizeof(text), "%.3f", fabs(speed));

  // Measure text width for center alignment
  int text_width = text_measure_width(font, text, font_size);

  // Calculate text center position at radius distance from crosshair center
  int text_center_x = (speed < 0) ? (cx - SPEED_RADIUS_HORIZONTAL)  // LEFT
                                  : (cx + SPEED_RADIUS_HORIZONTAL); // RIGHT
  int text_center_y = cy;

  // Position so text center is at the calculated position
  int x = text_center_x - (text_width / 2);
  int y = text_center_y - (font_size / 2);

  text_render_with_outline(fb, font, text, x, y, color,
                           0xFF000000,  // Black outline
                           font_size, 1 // 1px outline
  );
}

// Render elevation speed indicator (top/bottom of crosshair)
// Matches web version: wasm_osd_day_lib.c:211-233
static void
render_elevation_speed(framebuffer_t *fb,
                       const font_resource_t *font,
                       int cx,
                       int cy,
                       double speed,
                       uint32_t color,
                       int font_size)
{
  char text[32];
  snprintf(text, sizeof(text), "%.3f", fabs(speed));

  // Measure text width for center alignment
  int text_width = text_measure_width(font, text, font_size);

  // Calculate text center position at radius distance from crosshair center
  int text_center_x = cx;
  int text_center_y = (speed < 0) ? (cy + SPEED_RADIUS_VERTICAL)  // BELOW
                                  : (cy - SPEED_RADIUS_VERTICAL); // ABOVE

  // Position so text center is at the calculated position
  int x = text_center_x - (text_width / 2);
  int y = text_center_y - (font_size / 2);

  text_render_with_outline(fb, font, text, x, y, color,
                           0xFF000000,  // Black outline
                           font_size, 1 // 1px outline
  );
}

// Main speed indicator rendering function
static void
render_speed_indicators(osd_context_t *ctx,
                        osd_state_t *pb_state,
                        int cx,
                        int cy)
{
  if (!ctx->config.speed_indicators.enabled)
    return;
  if (!pb_state || !pb_state->has_rotary)
    return;

  framebuffer_t fb = ctx_to_framebuffer(ctx);

  // Get rotary speeds from proto (already normalized: -1.0 to 1.0)
  double az_speed_normalized = pb_state->rotary.azimuth_speed;
  double el_speed_normalized = pb_state->rotary.elevation_speed;
  bool is_moving             = pb_state->rotary.is_moving;

  // Config values
  float threshold = ctx->config.speed_indicators.threshold;
  float max_az    = ctx->config.speed_indicators.max_speed_azimuth;
  float max_el    = ctx->config.speed_indicators.max_speed_elevation;

  // Threshold check uses normalized value directly (already 0.0 to 1.0)
  bool show_az = fabs(az_speed_normalized) > threshold;
  bool show_el = fabs(el_speed_normalized) > threshold;

  // Convert to degrees for display: normalized * max_speed
  double az_speed_degrees = az_speed_normalized * max_az;
  double el_speed_degrees = el_speed_normalized * max_el;

  // If not moving, clear all positions and return
  if (!is_moving || (!show_az && !show_el))
    {
      clear_speed_indicators(&fb, cx, cy,
                             ctx->config.speed_indicators.font_size);
      return;
    }

  // Render speed indicators (only when moving) - display in degrees
  if (show_az)
    {
      render_azimuth_speed(&fb, &ctx->font_speed_indicators, cx, cy,
                           az_speed_degrees, ctx->config.speed_indicators.color,
                           ctx->config.speed_indicators.font_size);
    }

  if (show_el)
    {
      render_elevation_speed(&fb, &ctx->font_speed_indicators, cx, cy,
                             el_speed_degrees,
                             ctx->config.speed_indicators.color,
                             ctx->config.speed_indicators.font_size);
    }
}

// ════════════════════════════════════════════════════════════
// MAIN CROSSHAIR RENDERING
// ════════════════════════════════════════════════════════════

bool
crosshair_render(osd_context_t *ctx, osd_state_t *pb_state)
{
  if (!ctx->config.crosshair.enabled)
    return false;

  // Calculate center with offset from state (user aim position)
  // Use heat or day crosshair offset depending on active camera
  int offset_x = pb_state->rec_osd.heat_osd_enabled
                   ? pb_state->rec_osd.heat_crosshair_offset_horizontal
                   : pb_state->rec_osd.day_crosshair_offset_horizontal;
  int offset_y = pb_state->rec_osd.heat_osd_enabled
                   ? pb_state->rec_osd.heat_crosshair_offset_vertical
                   : pb_state->rec_osd.day_crosshair_offset_vertical;
  int cx       = ctx->width / 2 + offset_x;
  int cy       = ctx->height / 2 + offset_y;

  framebuffer_t fb = ctx_to_framebuffer(ctx);

  // Render in order: circle, cross, center dot, speed indicators
  crosshair_render_circle(&fb, &ctx->config.crosshair, cx, cy);
  crosshair_render_cross(&fb, &ctx->config.crosshair, cx, cy);
  crosshair_render_center_dot(&fb, &ctx->config.crosshair, cx, cy);

  // Render speed indicators
  render_speed_indicators(ctx, pb_state, cx, cy);

  return true;
}
