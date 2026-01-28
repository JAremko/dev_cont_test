// OSD State Accessors Implementation
// Extracts data from protobuf state for widget consumption

#include "osd_state.h"

#include "core/osd_context.h"
#include "proto/jon_shared_data.pb.h"

// ════════════════════════════════════════════════════════════
// ORIENTATION DATA
// ════════════════════════════════════════════════════════════

bool
osd_state_get_orientation(const osd_state_t *state,
                          double *azimuth,
                          double *elevation,
                          double *bank)
{
  if (!state || !state->has_compass)
    return false;

  if (azimuth)
    *azimuth = state->compass.azimuth;
  if (elevation)
    *elevation = state->compass.elevation;
  if (bank)
    *bank = state->compass.bank;

  return true;
}

// ════════════════════════════════════════════════════════════
// SPEED DATA
// ════════════════════════════════════════════════════════════

bool
osd_state_get_speeds(const osd_state_t *state,
                     double *azimuth_speed,
                     double *elevation_speed,
                     bool *is_moving)
{
  if (!state || !state->has_rotary)
    return false;

  if (azimuth_speed)
    *azimuth_speed = state->rotary.azimuth_speed;
  if (elevation_speed)
    *elevation_speed = state->rotary.elevation_speed;
  if (is_moving)
    *is_moving = state->rotary.is_moving;

  return true;
}

// ════════════════════════════════════════════════════════════
// CROSSHAIR OFFSET
// ════════════════════════════════════════════════════════════

void
osd_state_get_crosshair_offset(const osd_state_t *state,
                               bool is_thermal_stream,
                               int *offset_x,
                               int *offset_y)
{
  // Default to center (no offset)
  int x = 0;
  int y = 0;

  if (state && state->has_rec_osd)
    {
      if (is_thermal_stream)
        {
          x = state->rec_osd.heat_crosshair_offset_horizontal;
          y = state->rec_osd.heat_crosshair_offset_vertical;
        }
      else
        {
          x = state->rec_osd.day_crosshair_offset_horizontal;
          y = state->rec_osd.day_crosshair_offset_vertical;
        }
    }

  if (offset_x)
    *offset_x = x;
  if (offset_y)
    *offset_y = y;
}

// ════════════════════════════════════════════════════════════
// TIME DATA
// ════════════════════════════════════════════════════════════

int64_t
osd_state_get_timestamp(const osd_state_t *state)
{
  if (!state || !state->has_time)
    return 0;

  return state->time.timestamp;
}

// ════════════════════════════════════════════════════════════
// GPS DATA
// ════════════════════════════════════════════════════════════

bool
osd_state_get_gps(const osd_state_t *state, osd_gps_position_t *pos)
{
  if (!pos)
    return false;

  // Initialize to invalid
  pos->valid     = false;
  pos->latitude  = 0.0;
  pos->longitude = 0.0;
  pos->altitude  = 0.0;
  pos->timestamp = 0;

  if (!state || !state->has_actual_space_time)
    return false;

  const ser_JonGuiDataActualSpaceTime *ast = &state->actual_space_time;

  pos->latitude  = ast->latitude;
  pos->longitude = ast->longitude;
  pos->altitude  = ast->altitude;
  pos->timestamp = ast->timestamp;
  pos->valid     = true;

  return true;
}

// ════════════════════════════════════════════════════════════
// STATE TIMING DATA
// ════════════════════════════════════════════════════════════

uint64_t
osd_state_get_monotonic_time_us(const osd_state_t *state)
{
  if (!state)
    return 0;

  return state->system_monotonic_time_us;
}

uint64_t
osd_state_get_frame_monotonic_day_us(const osd_state_t *state)
{
  if (!state)
    return 0;

  return state->frame_monotonic_day_us;
}

uint64_t
osd_state_get_frame_monotonic_heat_us(const osd_state_t *state)
{
  if (!state)
    return 0;

  return state->frame_monotonic_heat_us;
}

// ════════════════════════════════════════════════════════════
// CLIENT METADATA
// ════════════════════════════════════════════════════════════

bool
osd_state_get_client_metadata(const osd_context_t *ctx,
                              osd_client_metadata_t *metadata)
{
  // Check both pointers upfront before any dereference
  if (!metadata || !ctx)
    return false;

  // Initialize to invalid/zero
  metadata->valid              = false;
  metadata->canvas_width_px    = 0;
  metadata->canvas_height_px   = 0;
  metadata->device_pixel_ratio = 0.0f;
  metadata->osd_buffer_width   = 0;
  metadata->osd_buffer_height  = 0;

  // Proxy bounds
  metadata->video_proxy_ndc_x      = 0.0f;
  metadata->video_proxy_ndc_y      = 0.0f;
  metadata->video_proxy_ndc_width  = 0.0f;
  metadata->video_proxy_ndc_height = 0.0f;

  // Scale factor
  metadata->scale_factor = 0.0f;

  // Theme info
  metadata->is_sharp_mode   = false;
  metadata->theme_hue       = 0.0f;
  metadata->theme_chroma    = 0.0f;
  metadata->theme_lightness = 0.0f;

  if (!ctx->client_metadata.valid)
    return false;

  // Canvas info
  metadata->canvas_width_px    = ctx->client_metadata.canvas_width_px;
  metadata->canvas_height_px   = ctx->client_metadata.canvas_height_px;
  metadata->device_pixel_ratio = ctx->client_metadata.device_pixel_ratio;
  metadata->osd_buffer_width   = ctx->client_metadata.osd_buffer_width;
  metadata->osd_buffer_height  = ctx->client_metadata.osd_buffer_height;

  // Proxy bounds
  metadata->video_proxy_ndc_x     = ctx->client_metadata.video_proxy_ndc_x;
  metadata->video_proxy_ndc_y     = ctx->client_metadata.video_proxy_ndc_y;
  metadata->video_proxy_ndc_width = ctx->client_metadata.video_proxy_ndc_width;
  metadata->video_proxy_ndc_height
    = ctx->client_metadata.video_proxy_ndc_height;

  // Scale factor
  metadata->scale_factor = ctx->client_metadata.scale_factor;

  // Theme info
  metadata->is_sharp_mode   = ctx->client_metadata.is_sharp_mode;
  metadata->theme_hue       = ctx->client_metadata.theme_hue;
  metadata->theme_chroma    = ctx->client_metadata.theme_chroma;
  metadata->theme_lightness = ctx->client_metadata.theme_lightness;

  metadata->valid = true;

  return true;
}
