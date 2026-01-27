/**
 * @file variant_info.c
 * @brief Variant information widget implementation
 */

#include "widgets/variant_info.h"

#include "core/framebuffer.h"
#include "osd_state.h"
#include "rendering/text.h"
#include "utils/logging.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Math function declarations for WASI SDK compatibility
#include "utils/math_decl.h"

// Variant info layout constants
#define VARIANT_INFO_LINE_SPACING 4      // Vertical spacing between lines
#define VARIANT_INFO_OUTLINE_THICKNESS 1 // Outline thickness for text

// Delta averaging constants
#define DELTA_HISTORY_SIZE 150    // ~5 seconds at 30fps
#define DELTA_WINDOW_US 5000000UL // 5 seconds in microseconds

// Ring buffer for delta averaging
static struct
{
  double delta_ms[DELTA_HISTORY_SIZE];
  uint64_t timestamp_us[DELTA_HISTORY_SIZE];
  int write_idx;
  int count;
} delta_history = { 0 };

// Build info defaults (set by build.sh via -D defines)
#ifndef OSD_VERSION
#define OSD_VERSION "unknown"
#endif
#ifndef OSD_GIT_COMMIT
#define OSD_GIT_COMMIT "unknown"
#endif
#ifndef OSD_BUILD_DATE
#define OSD_BUILD_DATE "unknown"
#endif
#ifndef OSD_BUILD_TIME
#define OSD_BUILD_TIME "unknown"
#endif

/**
 * Add a delta sample to the history buffer.
 * Auto-resets if there's been a gap (worker was inactive).
 */
static void
delta_history_add(double delta_ms, uint64_t timestamp_us)
{
  // Detect gap: if newest sample is older than our window, reset history
  // This handles worker reactivation after being disabled
  if (delta_history.count > 0)
    {
      // Find the most recent sample (one before write_idx)
      int last_idx = (delta_history.write_idx - 1 + DELTA_HISTORY_SIZE)
                     % DELTA_HISTORY_SIZE;
      uint64_t last_ts = delta_history.timestamp_us[last_idx];

      // If last sample is older than window, reset (worker was inactive)
      if (timestamp_us > last_ts + DELTA_WINDOW_US)
        {
          delta_history.count     = 0;
          delta_history.write_idx = 0;
        }
    }

  delta_history.delta_ms[delta_history.write_idx]     = delta_ms;
  delta_history.timestamp_us[delta_history.write_idx] = timestamp_us;
  delta_history.write_idx = (delta_history.write_idx + 1) % DELTA_HISTORY_SIZE;
  if (delta_history.count < DELTA_HISTORY_SIZE)
    {
      delta_history.count++;
    }
}

/**
 * Calculate average and standard deviation of delta over the last 5 seconds
 * @param current_us Current monotonic time in microseconds
 * @param out_avg Output: average delta in ms
 * @param out_std Output: standard deviation in ms
 * @param out_count Output: number of samples in window
 * @return true if stats are available, false if no samples in window
 */
static bool
delta_history_stats(uint64_t current_us,
                    double *out_avg,
                    double *out_std,
                    int *out_count)
{
  if (delta_history.count == 0)
    {
      return false;
    }

  uint64_t cutoff
    = (current_us > DELTA_WINDOW_US) ? (current_us - DELTA_WINDOW_US) : 0;

  // First pass: calculate mean
  double sum      = 0.0;
  int valid_count = 0;
  for (int i = 0; i < delta_history.count; i++)
    {
      if (delta_history.timestamp_us[i] >= cutoff)
        {
          sum += delta_history.delta_ms[i];
          valid_count++;
        }
    }

  if (valid_count == 0)
    {
      return false;
    }

  double mean = sum / (double)valid_count;

  // Second pass: calculate variance
  double variance_sum = 0.0;
  for (int i = 0; i < delta_history.count; i++)
    {
      if (delta_history.timestamp_us[i] >= cutoff)
        {
          double diff = delta_history.delta_ms[i] - mean;
          variance_sum += diff * diff;
        }
    }

  *out_avg   = mean;
  *out_std   = sqrt(variance_sum / (double)valid_count);
  *out_count = valid_count;
  return true;
}

// Determine variant name from compile-time defines
static const char *
get_variant_name(void)
{
#if defined(OSD_MODE_LIVE) && defined(OSD_STREAM_DAY)
  return "live_day";
#elif defined(OSD_MODE_LIVE) && defined(OSD_STREAM_THERMAL)
  return "live_thermal";
#elif defined(OSD_MODE_RECORDING) && defined(OSD_STREAM_DAY)
  return "recording_day";
#elif defined(OSD_MODE_RECORDING) && defined(OSD_STREAM_THERMAL)
  return "recording_thermal";
#else
  return "unknown";
#endif
}

// ════════════════════════════════════════════════════════════
// WIDGET LIFECYCLE FUNCTIONS
// ════════════════════════════════════════════════════════════
//
// The variant info widget follows the standard widget pattern with
// init/render/cleanup functions for API consistency, but unlike other
// widgets (navball, font), it requires no resource allocation:
//
//   - No textures to load (pure text rendering)
//   - No lookup tables to precompute
//   - No file I/O required
//   - All data comes from compile-time defines or runtime config
//
// Therefore, init() and cleanup() are no-ops that simply log for
// debugging purposes. This pattern maintains a consistent widget API
// while avoiding unnecessary complexity.
//
// ════════════════════════════════════════════════════════════

/**
 * Initialize variant info widget
 *
 * This is a no-op because the variant info widget requires no resource
 * allocation. All rendering is done with existing font resources and
 * compile-time/runtime configuration data.
 *
 * @param ctx OSD context (unused)
 */
void
variant_info_init(osd_context_t *ctx)
{
  (void)ctx;
  LOG_INFO("Variant info widget initialized");
}

/**
 * Render variant info widget
 *
 * NOTE: When enabled, this widget ALWAYS returns true because it displays
 * the draw counter (frame_count) which changes on every state update.
 * This forces a texture re-upload every frame when variant_info is visible,
 * which is intentional for debugging purposes.
 *
 * @param ctx OSD context
 * @param state Proto state (used for monotonic time)
 * @return true if rendered (always when enabled), false if disabled
 */
bool
variant_info_render(osd_context_t *ctx, const osd_state_t *state)
{
  if (!ctx->config.variant_info.enabled)
    {
      return false;
    }

  framebuffer_t fb;
  framebuffer_init(&fb, ctx->framebuffer, ctx->width, ctx->height);

  int x                 = ctx->config.variant_info.pos_x;
  int y                 = ctx->config.variant_info.pos_y;
  const uint32_t color  = ctx->config.variant_info.color;
  const int font_size   = ctx->config.variant_info.font_size;
  const int line_height = font_size + VARIANT_INFO_LINE_SPACING;

  // Buffer for text rendering
  char buffer[256];

  // Render variant name header
  const char *variant_name = get_variant_name();
  snprintf(buffer, sizeof(buffer), "Variant: %s", variant_name);
  text_render_with_outline(&fb, &ctx->font_variant_info, buffer, x, y, color,
                           0xFF000000, // Black outline
                           font_size,  //
                           VARIANT_INFO_OUTLINE_THICKNESS);

  y += line_height;

  // Separator line
  y += VARIANT_INFO_LINE_SPACING;

  // Get speed data from state (always, for debug display)
  double az_speed = 0.0, el_speed = 0.0;
  bool is_moving = false;
  osd_state_get_speeds(state, &az_speed, &el_speed, &is_moving);

  // Render config values
  // Create items array and fill in values
  struct
  {
    const char *key;
    char value[128];
  } items[20];

  // Draw counter (increments each state update/render cycle)
  snprintf(items[0].value, sizeof(items[0].value), "%u", ctx->frame_count);
  items[0].key = "Draw Count";

  // State timing info
  uint64_t monotonic_us = osd_state_get_monotonic_time_us(state);
  snprintf(items[1].value, sizeof(items[1].value), "%" PRIu64 " us",
           monotonic_us);
  items[1].key = "State Time";

  // Frame timing delta (shows frame age relative to state time)
#ifdef OSD_STREAM_THERMAL
  uint64_t frame_us       = osd_state_get_frame_monotonic_heat_us(state);
  const char *frame_label = "Heat Frame dt";
#else
  uint64_t frame_us       = osd_state_get_frame_monotonic_day_us(state);
  const char *frame_label = "Day Frame dt";
#endif
  if (frame_us > 0 && monotonic_us > 0)
    {
      // Delta in microseconds (positive = frame is older than state)
      int64_t delta_us = (int64_t)monotonic_us - (int64_t)frame_us;
      double delta_ms  = (double)delta_us / 1000.0;

      // Add to history for stats
      delta_history_add(delta_ms, monotonic_us);

      // Get stats over last 5 seconds
      double avg_ms, std_ms;
      int sample_count;
      if (delta_history_stats(monotonic_us, &avg_ms, &std_ms, &sample_count))
        {
          // Zero-padded fixed-width format for stable display
          // Sign + zero-pad ensures consistent width regardless of font
          snprintf(items[2].value, sizeof(items[2].value),
                   "%+08.2f (avg %+08.2f std %07.2f n=%03d)", delta_ms, avg_ms,
                   std_ms, sample_count);
        }
      else
        {
          snprintf(items[2].value, sizeof(items[2].value), "%+08.2f ms",
                   delta_ms);
        }
    }
  else
    {
      snprintf(items[2].value, sizeof(items[2].value), "N/A");
    }
  items[2].key = frame_label;

  snprintf(items[3].value, sizeof(items[3].value), "%ux%u", ctx->width,
           ctx->height);
  items[3].key = "Resolution";

#ifdef OSD_MODE_LIVE
  snprintf(items[4].value, sizeof(items[4].value), "Live");
#else
  snprintf(items[4].value, sizeof(items[4].value), "Recording");
#endif
  items[4].key = "Mode";

  snprintf(items[5].value, sizeof(items[5].value), "%s",
           ctx->config.crosshair.enabled ? "Enabled" : "Disabled");
  items[5].key = "Crosshair";

  snprintf(items[6].value, sizeof(items[6].value), "%s",
           ctx->config.timestamp.enabled ? "Enabled" : "Disabled");
  items[6].key = "Timestamp";

  snprintf(items[7].value, sizeof(items[7].value), "%s",
           ctx->config.speed_indicators.enabled ? "Enabled" : "Disabled");
  items[7].key = "Speed Indicators";

  snprintf(items[8].value, sizeof(items[8].value), "%s",
           ctx->config.navball.enabled ? "Enabled" : "Disabled");
  items[8].key = "Navball";

  snprintf(items[9].value, sizeof(items[9].value), "%d, %d",
           ctx->config.navball.position_x, ctx->config.navball.position_y);
  items[9].key = "Navball Pos";

  snprintf(items[10].value, sizeof(items[10].value), "%dpx",
           ctx->config.navball.size);
  items[10].key = "Navball Size";

  // Speed debug info (always shown)
  // Speeds from proto are normalized (-1.0 to 1.0)
  // Display both normalized and degrees (normalized * 35.0)
  snprintf(items[11].value, sizeof(items[11].value), "%s",
           is_moving ? "YES" : "NO");
  items[11].key = "Is Moving";

  snprintf(items[12].value, sizeof(items[12].value), "%.3f (%.1f deg)",
           az_speed, az_speed * 35.0);
  items[12].key = "Az Speed";

  snprintf(items[13].value, sizeof(items[13].value), "%.3f (%.1f deg)",
           el_speed, el_speed * 35.0);
  items[13].key = "El Speed";

  // Build info (compile-time constants)
  snprintf(items[14].value, sizeof(items[14].value), "%s", OSD_VERSION);
  items[14].key = "Version";

  snprintf(items[15].value, sizeof(items[15].value), "%s", OSD_GIT_COMMIT);
  items[15].key = "Commit";

  snprintf(items[16].value, sizeof(items[16].value), "%s %s UTC",
           OSD_BUILD_DATE, OSD_BUILD_TIME);
  items[16].key = "Built";

  // Client metadata (canvas info from frontend via opaque payload)
  osd_client_metadata_t client_meta;
  if (osd_state_get_client_metadata(ctx, &client_meta) && client_meta.valid)
    {
      snprintf(items[17].value, sizeof(items[17].value), "%ux%u",
               client_meta.canvas_width_px, client_meta.canvas_height_px);
      items[17].key = "Canvas";

      snprintf(items[18].value, sizeof(items[18].value), "%.2f",
               client_meta.device_pixel_ratio);
      items[18].key = "DPR";

      snprintf(items[19].value, sizeof(items[19].value), "%ux%u",
               client_meta.osd_buffer_width, client_meta.osd_buffer_height);
      items[19].key = "OSD Buffer";
    }
  else
    {
      snprintf(items[17].value, sizeof(items[17].value), "N/A");
      items[17].key = "Canvas";

      snprintf(items[18].value, sizeof(items[18].value), "N/A");
      items[18].key = "DPR";

      snprintf(items[19].value, sizeof(items[19].value), "N/A");
      items[19].key = "OSD Buffer";
    }

  // Render each config item
  for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
    {
      snprintf(buffer, sizeof(buffer), "%s: %s", items[i].key, items[i].value);
      text_render_with_outline(&fb, &ctx->font_variant_info, buffer, x, y,
                               color,
                               0xFF000000, // Black outline
                               font_size,  //
                               VARIANT_INFO_OUTLINE_THICKNESS);

      y += line_height;
    }

  // Render redraw warning at bottom
  text_render_with_outline(&fb, &ctx->font_variant_info, "[FORCES REPAINTS]", x,
                           y, color,
                           0xFF000000, // Black outline
                           font_size,  //
                           VARIANT_INFO_OUTLINE_THICKNESS);

  return true;
}

/**
 * Clean up variant info widget
 *
 * This is a no-op because the variant info widget allocates no resources.
 * Exists for API consistency with other widgets.
 *
 * @param ctx OSD context (unused)
 */
void
variant_info_cleanup(osd_context_t *ctx)
{
  (void)ctx;
  LOG_INFO("Variant info widget cleaned up");
}
