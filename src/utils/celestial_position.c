/**
 * @file celestial_position.c
 * @brief Implementation of celestial body position calculations
 *
 * This module wraps the Astronomy Engine library to provide a simple API
 * for calculating Sun and Moon positions in horizontal coordinates.
 *
 * KEY CONVERSIONS:
 * - Unix timestamp → J2000 days: (unix_seconds / 86400.0) - 10957.5
 * - J2000 epoch: 2000-01-01 12:00:00 UTC (noon)
 * - Unix epoch:  1970-01-01 00:00:00 UTC (midnight)
 * - Difference: 10957.5 days
 *
 * ALGORITHM FLOW:
 * 1. Convert Unix timestamp to astro_time_t (J2000 days)
 * 2. Create astro_observer_t from GPS coordinates
 * 3. Call Astronomy_Equator() to get RA/Dec
 * 4. Call Astronomy_Horizon() to convert to azimuth/altitude
 * 5. Apply atmospheric refraction correction
 */

#include "celestial_position.h"

#include "astronomy.h"
#include "utils/logging.h"
#include "utils/math_decl.h" /* Math declarations for WASI SDK + cglm */

#include <string.h>

/* cglm - Optimized C graphics math library (quaternions, matrices) */
#include <cglm/cglm.h>

/* ════════════════════════════════════════════════════════════
 * CONSTANTS
 * ════════════════════════════════════════════════════════════ */

/**
 * @def SECONDS_PER_DAY
 * @brief Number of seconds in one day (86400 = 24 * 60 * 60)
 */
#define SECONDS_PER_DAY 86400.0

/**
 * @def UNIX_EPOCH_TO_J2000_DAYS
 * @brief Days between Unix epoch (1970-01-01) and J2000 epoch (2000-01-01 noon)
 *
 * Calculation:
 * - Years: 2000 - 1970 = 30 years
 * - Leap years: 1972, 1976, 1980, 1984, 1988, 1992, 1996 = 7 days
 * - Regular days: 30 * 365 = 10950 days
 * - J2000 noon offset: +0.5 days
 * - Total: 10950 + 7 + 0.5 = 10957.5 days
 */
#define UNIX_EPOCH_TO_J2000_DAYS 10957.5

/* ════════════════════════════════════════════════════════════
 * PRIVATE HELPER FUNCTIONS
 * ════════════════════════════════════════════════════════════ */

/**
 * @brief Convert Unix timestamp to Astronomy Engine time
 *
 * Converts a Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
 * to an astro_time_t value (days since 2000-01-01 12:00:00 UTC).
 *
 * @param unix_timestamp Seconds since Unix epoch
 * @return Astronomy Engine time structure
 */
static astro_time_t
unix_to_astro_time(int64_t unix_timestamp)
{
  double days_since_j2000
    = (unix_timestamp / SECONDS_PER_DAY) - UNIX_EPOCH_TO_J2000_DAYS;
  return Astronomy_TimeFromDays(days_since_j2000);
}

/**
 * @brief Calculate position of a celestial body
 *
 * This is the core calculation function that:
 * 1. Converts observer location to astro_observer_t
 * 2. Gets equatorial coordinates (RA/Dec) for the body
 * 3. Converts to horizontal coordinates (azimuth/altitude)
 * 4. Applies atmospheric refraction correction
 *
 * @param body Celestial body (BODY_SUN or BODY_MOON)
 * @param time Astronomy Engine time
 * @param observer Observer location
 * @return Horizontal position (azimuth, altitude, valid flag)
 */
static celestial_position_t
calculate_body_position(astro_body_t body,
                        astro_time_t *time,
                        observer_location_t observer)
{
  celestial_position_t result = { .valid = false };

  /* Create Astronomy Engine observer structure */
  astro_observer_t astro_observer = Astronomy_MakeObserver(
    observer.latitude, observer.longitude, observer.altitude);

  /* Get equatorial coordinates (Right Ascension, Declination)
   * - EQUATOR_OF_DATE: Use true equator at observation time (accounts for
   * precession)
   * - ABERRATION: Apply aberration correction (accounts for Earth's orbital
   * motion) */
  astro_equatorial_t equ = Astronomy_Equator(body, time, astro_observer,
                                             EQUATOR_OF_DATE, ABERRATION);

  if (equ.status != ASTRO_SUCCESS)
    {
      LOG_WARN("Failed to calculate equatorial coordinates for body %d: "
               "status=%d",
               (int)body, equ.status);
      return result;
    }

  /* Convert equatorial coordinates to horizontal coordinates
   * - REFRACTION_NORMAL: Apply standard atmospheric refraction correction
   * - This accounts for the bending of light through Earth's atmosphere */
  astro_horizon_t hor = Astronomy_Horizon(time, astro_observer, equ.ra, equ.dec,
                                          REFRACTION_NORMAL);

  /* Populate result */
  result.azimuth  = hor.azimuth;  /* 0=North, 90=East, 180=South, 270=West */
  result.altitude = hor.altitude; /* +90=zenith, 0=horizon, -90=nadir */
  result.valid    = true;

  return result;
}

/* ════════════════════════════════════════════════════════════
 * PUBLIC API IMPLEMENTATION
 * ════════════════════════════════════════════════════════════ */

bool
celestial_init(void)
{
  /* Astronomy Engine is a header-only library with no initialization
   * required. This function is provided for API completeness and future
   * extensibility (e.g., caching, LUT precomputation). */
  LOG_INFO("Celestial position system initialized");
  return true;
}

celestial_positions_t
celestial_calculate(int64_t unix_timestamp, observer_location_t observer)
{
  celestial_positions_t result = { .sun.valid = false, .moon.valid = false };

  /* Convert Unix timestamp to Astronomy Engine time */
  astro_time_t time = unix_to_astro_time(unix_timestamp);

  /* Calculate Sun position */
  result.sun = calculate_body_position(BODY_SUN, &time, observer);

  /* Calculate Moon position */
  result.moon = calculate_body_position(BODY_MOON, &time, observer);

  return result;
}

void
celestial_cleanup(void)
{
  /* No cleanup required for Astronomy Engine library */
  LOG_INFO("Celestial position system cleaned up");
}

/* ════════════════════════════════════════════════════════════
 * COORDINATE TRANSFORMATION HELPERS
 * ════════════════════════════════════════════════════════════ */

/**
 * @def DEG_TO_RAD
 * @brief Convert degrees to radians
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG_TO_RAD(deg) ((deg)*M_PI / 180.0)

/**
 * @brief 3D vector structure
 */
typedef struct
{
  double x, y, z;
} vec3_t;

/**
 * @brief 3x3 rotation matrix
 */
typedef struct
{
  double m[9]; // Row-major: m[row*3 + col]
} mat3_t;

/**
 * @brief Convert horizontal coordinates to 3D unit vector
 *
 * Converts azimuth (compass direction) and altitude (elevation angle)
 * to a 3D unit vector in the horizontal coordinate frame.
 *
 * Coordinate system:
 * - x-axis: East (azimuth 90°)
 * - y-axis: Up (altitude 90°)
 * - z-axis: North (azimuth 0°)
 *
 * @param azimuth Compass direction in degrees (0=North, 90=East)
 * @param altitude Elevation angle in degrees (0=horizon, 90=zenith)
 * @return 3D unit vector
 */
static vec3_t
horizontal_to_vector(double azimuth, double altitude)
{
  double az_rad  = DEG_TO_RAD(azimuth);
  double alt_rad = DEG_TO_RAD(altitude);

  double cos_alt = cos(alt_rad);

  vec3_t v;
  v.x = cos_alt * sin(az_rad); // East component
  v.y = sin(alt_rad);          // Up component
  v.z = cos_alt * cos(az_rad); // North component

  return v;
}

/**
 * @brief Create rotation matrix from Euler angles (QUATERNION-BASED)
 *
 * Creates a 3x3 rotation matrix from Euler angles (azimuth, elevation, bank)
 * using gimbal-lock-free quaternion conversion with YXZ intrinsic order via
 * cglm library.
 *
 * ROTATION ORDER (YXZ intrinsic):
 *   1. Pitch (elevation) - rotates around lateral axis
 *   2. Roll (azimuth) - rotates around longitudinal axis (SWAPPED)
 *   3. Yaw (bank) - rotates around vertical axis (SWAPPED)
 *
 * This order ensures proper axis independence and prevents gimbal lock
 * in the -45° to +45° pitch range.
 *
 * AXIS MAPPING: Azimuth and bank are swapped to correct rotation behavior.
 *
 * @param azimuth Platform heading in degrees (0=North, 90=East)
 * @param elevation Platform pitch in degrees (nose up/down)
 * @param bank Platform roll in degrees (wing tilt)
 * @return 3x3 rotation matrix
 */
static mat3_t
create_rotation_matrix(double azimuth, double elevation, double bank)
{
  /* Convert degrees to radians (remove negations to fix direction inversions)
   */
  double pitch_rad = DEG_TO_RAD(elevation); // Platform up → sphere up
  double roll_rad  = DEG_TO_RAD(azimuth);   // SWAPPED: azimuth → roll axis
  double yaw_rad   = DEG_TO_RAD(bank);      // SWAPPED: bank → yaw axis

  /* Build rotation matrix using cglm library (gimbal-lock-free quaternion
   * conversion) YXZ intrinsic order: pitch first (angles[0]), then roll
   * (angles[1]), then yaw (angles[2]) */

  /* Note: cglm uses float, so we need to cast from double */
  vec3 angles = { (float)pitch_rad, (float)roll_rad,
                  (float)yaw_rad }; // cglm vec3 is float[3]
  versor q;                         // cglm versor is float[4]
  mat3 cglm_mat3;                   // cglm mat3 is float[9]

  glm_euler_yxz_quat(angles, q);    // Convert YXZ euler angles to quaternion
  glm_quat_mat3(q, cglm_mat3);      // Convert quaternion to 3×3 matrix

  /* Copy cglm mat3 to our local mat3_t struct, converting float to double */
  mat3_t m;
  for (int i = 0; i < 9; i++)
    {
      m.m[i] = (double)cglm_mat3[i / 3][i % 3]; // cglm mat3 is mat3[3][3]
    }

  return m;
}

/**
 * @brief Multiply 3x3 matrix by 3D vector
 *
 * Performs matrix-vector multiplication: result = m * v
 *
 * @param m 3x3 rotation matrix (row-major)
 * @param v 3D input vector
 * @return Transformed 3D vector
 */
static vec3_t
mat3_mul_vec3(mat3_t m, vec3_t v)
{
  vec3_t result;
  result.x = m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z;
  result.y = m.m[3] * v.x + m.m[4] * v.y + m.m[5] * v.z;
  result.z = m.m[6] * v.x + m.m[7] * v.y + m.m[8] * v.z;
  return result;
}

bool
celestial_to_navball_coords(double azimuth,
                            double altitude,
                            double platform_azimuth,
                            double platform_elevation,
                            double platform_bank,
                            int navball_center_x,
                            int navball_center_y,
                            int navball_radius,
                            int *screen_x,
                            int *screen_y)
{
  /* Step 1: Convert celestial azimuth/altitude to 3D unit vector */
  vec3_t celestial_vec = horizontal_to_vector(azimuth, altitude);

  /* Step 2: Apply platform rotation (inverse transform) */
  mat3_t rotation = create_rotation_matrix(platform_azimuth, platform_elevation,
                                           platform_bank);
  vec3_t rotated  = mat3_mul_vec3(rotation, celestial_vec);

  /* Step 3: Check visibility (front vs back hemisphere)
   * If z > 0, the celestial body is on the front of the navball (visible)
   * If z < 0, it's behind the navball */
  bool is_front = (rotated.z > 0.0);

  /* Step 4: Project onto navball screen coordinates
   * The rotated x, y components directly map to screen offsets
   * Scale by navball radius and add navball center */
  *screen_x = navball_center_x + (int)(rotated.x * navball_radius);
  *screen_y = navball_center_y - (int)(rotated.y * navball_radius); // Invert Y
                                                                    // (screen
                                                                    // coords)

  return is_front;
}
