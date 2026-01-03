/**
 * @file synthetic_state.c
 * @brief Synthetic protobuf state generator implementation
 */

#include "synthetic_state.h"
#include "jon_shared_data.pb.h"
#include "pb_encode.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Speed values are normalized (-1.0 to 1.0)
// Display conversion: degrees = normalized * max_speed (from config)
// Example: 0.5 normalized * 35.0 max_speed = 17.5 degrees displayed

synthetic_state_t *
synthetic_state_create (animation_type_t type,
                        float duration_seconds,
                        uint32_t fps)
{
  printf ("[SYNTHETIC_STATE] Creating generator\n");
  printf ("[SYNTHETIC_STATE]   Type: %d\n", type);
  printf ("[SYNTHETIC_STATE]   Duration: %.1f seconds\n", duration_seconds);
  printf ("[SYNTHETIC_STATE]   FPS: %u\n", fps);

  synthetic_state_t *gen = calloc (1, sizeof (synthetic_state_t));
  if (!gen)
    {
      fprintf (stderr, "[SYNTHETIC_STATE] Failed to allocate generator\n");
      return NULL;
    }

  gen->type = type;
  gen->fps = fps;
  gen->total_frames = (uint32_t)(duration_seconds * fps);
  gen->current_frame = 0;
  gen->phase = 0.0f;

  // Allocate protobuf state
  gen->state = calloc (1, sizeof (ser_JonGUIState));
  if (!gen->state)
    {
      fprintf (stderr, "[SYNTHETIC_STATE] Failed to allocate state\n");
      free (gen);
      return NULL;
    }

  // Allocate encoding buffer (4KB should be enough)
  gen->buffer_size = 4096;
  gen->encoded_buffer = malloc (gen->buffer_size);
  if (!gen->encoded_buffer)
    {
      fprintf (stderr, "[SYNTHETIC_STATE] Failed to allocate buffer\n");
      free (gen->state);
      free (gen);
      return NULL;
    }

  // Initialize state with default values
  gen->state->has_compass = true;
  gen->state->compass.azimuth = 0.0;
  gen->state->compass.elevation = 0.0;
  gen->state->compass.bank = 0.0;

  gen->state->has_rotary = true;
  gen->state->rotary.azimuth_speed = 0.0;
  gen->state->rotary.elevation_speed = 0.0;
  gen->state->rotary.is_moving = false;

  // Use fixed timestamp: 2025-01-08 00:00:00 UTC (4:00 PM PST, Jan 7)
  // First Quarter Moon - both sun and moon well above horizon
  // Sun: ~20° altitude (late afternoon), Moon: ~40° altitude (eastern sky)
  int64_t fixed_timestamp = 1736294400;

  gen->state->has_time = true;
  gen->state->time.timestamp = fixed_timestamp;

  // Initialize GPS/location data for celestial indicators
  gen->state->has_actual_space_time = true;
  gen->state->actual_space_time.latitude = 37.7749;    // San Francisco
  gen->state->actual_space_time.longitude = -122.4194; // San Francisco
  gen->state->actual_space_time.altitude = 0.0;
  gen->state->actual_space_time.timestamp = fixed_timestamp;

  printf ("[SYNTHETIC_STATE] Generator created (%u frames)\n",
          gen->total_frames);
  return gen;
}

bool
synthetic_state_next_frame (synthetic_state_t *gen)
{
  if (!gen || gen->current_frame >= gen->total_frames)
    return false;

  // Update phase (0.0 to 1.0 over animation duration)
  gen->phase = (float)gen->current_frame / (float)gen->total_frames;

  // Update compass based on animation type
  switch (gen->type)
    {
    case ANIM_STATIC:
      // Static orientation (facing north, level)
      gen->state->compass.azimuth = 0.0;
      gen->state->compass.elevation = 0.0;
      gen->state->compass.bank = 0.0;
      gen->state->actual_space_time.azimuth = 0.0;
      gen->state->actual_space_time.elevation = 0.0;
      gen->state->actual_space_time.bank = 0.0;
      gen->state->rotary.azimuth_speed = 0.0;
      gen->state->rotary.elevation_speed = 0.0;
      gen->state->rotary.is_moving = false;
      break;

    case ANIM_ROTATE_YAW:
      // Full 360° rotation around vertical axis
      gen->state->compass.azimuth = gen->phase * 360.0;
      gen->state->compass.elevation = 0.0;
      gen->state->compass.bank = 0.0;
      gen->state->actual_space_time.azimuth = gen->phase * 360.0;
      gen->state->actual_space_time.elevation = 0.0;
      gen->state->actual_space_time.bank = 0.0;
      gen->state->rotary.azimuth_speed = 0.5; // 50% of max (displays as 17.5°)
      gen->state->rotary.elevation_speed = 0.0;
      gen->state->rotary.is_moving = true;
      break;

    case ANIM_ROTATE_PITCH:
      // Pitch up/down (-90° to +90°)
      {
        float elev = sinf (gen->phase * 2.0 * M_PI) * 90.0;
        gen->state->compass.azimuth = 0.0;
        gen->state->compass.elevation = elev;
        gen->state->compass.bank = 0.0;
        gen->state->actual_space_time.azimuth = 0.0;
        gen->state->actual_space_time.elevation = elev;
        gen->state->actual_space_time.bank = 0.0;
        gen->state->rotary.azimuth_speed = 0.0;
        gen->state->rotary.elevation_speed
            = cosf (gen->phase * 2.0 * M_PI) * 0.8; // ±80% (displays as ±28°)
        gen->state->rotary.is_moving = true;
      }
      break;

    case ANIM_ROTATE_ROLL:
      // Roll left/right (-180° to +180°)
      {
        float roll = sinf (gen->phase * 2.0 * M_PI) * 180.0;
        gen->state->compass.azimuth = 0.0;
        gen->state->compass.elevation = 0.0;
        gen->state->compass.bank = roll;
        gen->state->actual_space_time.azimuth = 0.0;
        gen->state->actual_space_time.elevation = 0.0;
        gen->state->actual_space_time.bank = roll;
        gen->state->rotary.azimuth_speed = 0.0;
        gen->state->rotary.elevation_speed = 0.0;
        gen->state->rotary.is_moving = true;
      }
      break;

    case ANIM_CIRCLE:
      // Circular motion (combined yaw + pitch)
      {
        float angle = gen->phase * 2.0 * M_PI;
        float az = gen->phase * 360.0;
        float elev = sinf (angle) * 45.0;
        float roll = cosf (angle * 2.0) * 15.0;
        gen->state->compass.azimuth = az;
        gen->state->compass.elevation = elev;
        gen->state->compass.bank = roll;
        gen->state->actual_space_time.azimuth = az;
        gen->state->actual_space_time.elevation = elev;
        gen->state->actual_space_time.bank = roll;
        gen->state->rotary.azimuth_speed = 0.5;            // 50% (displays as 17.5°)
        gen->state->rotary.elevation_speed
            = cosf (angle) * 0.6; // ±60% (displays as ±21°)
        gen->state->rotary.is_moving = true;
      }
      break;

    case ANIM_SPEED_PULSE:
      // Pulsing speed indicators (static orientation)
      {
        float pulse = sinf (gen->phase * 4.0 * M_PI); // 2 full cycles
        gen->state->compass.azimuth = 0.0;
        gen->state->compass.elevation = 0.0;
        gen->state->compass.bank = 0.0;
        gen->state->actual_space_time.azimuth = 0.0;
        gen->state->actual_space_time.elevation = 0.0;
        gen->state->actual_space_time.bank = 0.0;
        gen->state->rotary.azimuth_speed = pulse * 0.9;   // ±90% (displays as ±31.5°)
        gen->state->rotary.elevation_speed = pulse * 0.7; // ±70% (displays as ±24.5°)
        gen->state->rotary.is_moving = (fabs (pulse) > 0.1);
      }
      break;
    }

  // Update timestamp - advance 1 second per frame for visible ticking
  // At 30 FPS, 10 seconds of video = 300 seconds (5 minutes) of time advancement
  int64_t base_timestamp = 1736294400; // 2025-01-08 00:00:00 UTC
  int64_t current_timestamp = base_timestamp + gen->current_frame;
  gen->state->time.timestamp = current_timestamp;
  gen->state->actual_space_time.timestamp = current_timestamp;

  gen->current_frame++;
  return true;
}

const uint8_t *
synthetic_state_get_encoded (synthetic_state_t *gen, size_t *out_size)
{
  if (!gen || !out_size)
    return NULL;

  // Encode protobuf using nanopb
  pb_ostream_t stream
    = pb_ostream_from_buffer (gen->encoded_buffer, gen->buffer_size);

  bool status = pb_encode (&stream, ser_JonGUIState_fields, gen->state);
  if (!status)
    {
      fprintf (stderr, "[SYNTHETIC_STATE] Encoding failed: %s\n",
               PB_GET_ERROR (&stream));
      *out_size = 0;
      return NULL;
    }

  *out_size = stream.bytes_written;
  return gen->encoded_buffer;
}

void
synthetic_state_reset (synthetic_state_t *gen)
{
  if (!gen)
    return;

  gen->current_frame = 0;
  gen->phase = 0.0f;
  printf ("[SYNTHETIC_STATE] Generator reset\n");
}

void
synthetic_state_destroy (synthetic_state_t *gen)
{
  if (!gen)
    return;

  printf ("[SYNTHETIC_STATE] Destroying generator\n");

  free (gen->encoded_buffer);
  free (gen->state);
  free (gen);
}
