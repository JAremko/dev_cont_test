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

// Variant info layout constants
#define VARIANT_INFO_LINE_SPACING 4      // Vertical spacing between lines
#define VARIANT_INFO_OUTLINE_THICKNESS 1 // Outline thickness for text

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
  } items[16];

  // Draw counter (increments each state update/render cycle)
  snprintf(items[0].value, sizeof(items[0].value), "%u", ctx->frame_count);
  items[0].key = "Draw Count";

  // State timing info
  uint64_t monotonic_us = osd_state_get_monotonic_time_us(state);
  snprintf(items[1].value, sizeof(items[1].value), "%" PRIu64 " us",
           monotonic_us);
  items[1].key = "State Time";

  snprintf(items[2].value, sizeof(items[2].value), "%ux%u", ctx->width,
           ctx->height);
  items[2].key = "Resolution";

#ifdef OSD_MODE_LIVE
  snprintf(items[3].value, sizeof(items[3].value), "Live");
#else
  snprintf(items[3].value, sizeof(items[3].value), "Recording");
#endif
  items[3].key = "Mode";

  snprintf(items[4].value, sizeof(items[4].value), "%s",
           ctx->config.crosshair.enabled ? "Enabled" : "Disabled");
  items[4].key = "Crosshair";

  snprintf(items[5].value, sizeof(items[5].value), "%s",
           ctx->config.timestamp.enabled ? "Enabled" : "Disabled");
  items[5].key = "Timestamp";

  snprintf(items[6].value, sizeof(items[6].value), "%s",
           ctx->config.speed_indicators.enabled ? "Enabled" : "Disabled");
  items[6].key = "Speed Indicators";

  snprintf(items[7].value, sizeof(items[7].value), "%s",
           ctx->config.navball.enabled ? "Enabled" : "Disabled");
  items[7].key = "Navball";

  snprintf(items[8].value, sizeof(items[8].value), "%d, %d",
           ctx->config.navball.position_x, ctx->config.navball.position_y);
  items[8].key = "Navball Pos";

  snprintf(items[9].value, sizeof(items[9].value), "%dpx",
           ctx->config.navball.size);
  items[9].key = "Navball Size";

  // Speed debug info (always shown)
  // Speeds from proto are normalized (-1.0 to 1.0)
  // Display both normalized and degrees (normalized * 35.0)
  snprintf(items[10].value, sizeof(items[10].value), "%s",
           is_moving ? "YES" : "NO");
  items[10].key = "Is Moving";

  snprintf(items[11].value, sizeof(items[11].value), "%.3f (%.1f deg)",
           az_speed, az_speed * 35.0);
  items[11].key = "Az Speed";

  snprintf(items[12].value, sizeof(items[12].value), "%.3f (%.1f deg)",
           el_speed, el_speed * 35.0);
  items[12].key = "El Speed";

  // Build info (compile-time constants)
  snprintf(items[13].value, sizeof(items[13].value), "%s", OSD_VERSION);
  items[13].key = "Version";

  snprintf(items[14].value, sizeof(items[14].value), "%s", OSD_GIT_COMMIT);
  items[14].key = "Commit";

  snprintf(items[15].value, sizeof(items[15].value), "%s %s", OSD_BUILD_DATE,
           OSD_BUILD_TIME);
  items[15].key = "Built";

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
