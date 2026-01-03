// OSD State Accessors Implementation
// Extracts data from protobuf state for widget consumption

#include "osd_state.h"

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
