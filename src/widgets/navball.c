#include "widgets/navball.h"

#include "core/framebuffer.h"
#include "jon_shared_data.pb.h"
#include "rendering/blending.h"
#include "resources/svg.h"
#include "utils/celestial_position.h"
#include "utils/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Math function declarations for WASI SDK + cglm compatibility
// MUST be included before cglm to ensure math functions are visible to cglm
// headers
#include "utils/math_decl.h"

// cglm - Optimized C graphics math library (quaternions, matrices)
// Note: Must include after math_decl.h
#include <cglm/cglm.h>

// STB Image for PNG loading
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD // Disable SIMD for WASM compatibility
#include "stb_image.h"

// ════════════════════════════════════════════════════════════
// 3D MATH UTILITIES
// ════════════════════════════════════════════════════════════

// 3D Vector
typedef struct
{
  float x, y, z;
} vec3_t;

// 2D UV coordinates
typedef struct
{
  float u, v;
} vec2_t;

// 4x4 Matrix (column-major) - keeping local struct for compatibility
typedef struct
{
  float m[16];
} mat4_t;

/**
 * Convert degrees to radians
 *
 * This macro converts an angle from degrees (0-360) to radians (0-2π).
 * Used throughout navball rendering for trigonometric calculations.
 *
 * @param deg Angle in degrees
 * @return Angle in radians (deg × π / 180)
 */
#define DEG_TO_RAD(deg) ((deg) * M_PI / 180.0f)

// Vector operations
static inline vec3_t
vec3_new(float x, float y, float z)
{
  vec3_t v = { x, y, z };
  return v;
}

static inline float
vec3_dot(vec3_t a, vec3_t b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline float
vec3_length(vec3_t v)
{
  return sqrtf(vec3_dot(v, v));
}

static inline vec3_t
vec3_normalize(vec3_t v)
{
  float len = vec3_length(v);
  if (len > 0.0001f)
    {
      v.x /= len;
      v.y /= len;
      v.z /= len;
    }
  return v;
}

// Matrix-vector multiplication (treat vec3 as vec4 with w=1, ignore
// translation)
static inline vec3_t
mat4_mul_vec3(mat4_t m, vec3_t v)
{
  vec3_t result;
  result.x = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z;
  result.y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z;
  result.z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z;
  return result;
}

// ════════════════════════════════════════════════════════════
// QUATERNION ROTATION (GIMBAL-LOCK-FREE) - Using cglm library
// ════════════════════════════════════════════════════════════
//
// Quaternions provide a gimbal-lock-free way to represent 3D rotations.
// Unlike Euler angles, they work smoothly at all orientations.
//
// We use the cglm library (https://github.com/recp/cglm) for:
//   - Euler angle to quaternion conversion (glm_euler_yxz_quat)
//   - Quaternion to 4x4 matrix conversion (glm_quat_mat4)
//
// AEROSPACE ROTATION ORDER (YXZ intrinsic):
//   1. Pitch (elevation) - rotate around lateral axis
//   2. Roll (bank) - rotate around longitudinal axis
//   3. Yaw (azimuth) - rotate around vertical axis
//
// This order ensures proper axis independence and prevents gimbal lock
// in the -45° to +45° pitch range.

// ════════════════════════════════════════════════════════════
// SPHERE TO UV MAPPING (Equirectangular Projection)
// ════════════════════════════════════════════════════════════
//
// This function maps 3D points on a unit sphere to 2D texture coordinates
// using the equirectangular (cylindrical) projection - the same method used
// by KSP's navball system.
//
// PROJECTION MATH:
//   Given a 3D point (x, y, z) on the sphere:
//   - θ (theta) = longitude angle = atan2(x, z)  → range [-π, π]
//   - φ (phi)   = latitude angle  = asin(y)      → range [-π/2, π/2]
//
//   UV coordinates are normalized to [0, 1]:
//   - u = θ/(2π) + 0.5    → maps [-π, π] to [0, 1] (horizontal)
//   - v = -φ/π + 0.5      → maps [-π/2, π/2] to [1, 0] (vertical, inverted)
//
// WHY INVERT V?
//   Texture V increases downward (top=0, bottom=1), but latitude increases
//   upward (north=+90°, south=-90°). Negating φ flips the vertical axis
//   so the texture maps correctly to the sphere.
//
// TEXTURE FORMAT:
//   Equirectangular textures have 2:1 aspect ratio (e.g., 1024x512)
//   - Full 360° horizontal wraparound
//   - 180° vertical coverage (pole to pole)
//
// PERFORMANCE:
//   This function is called ~70,000 times per frame (once per visible pixel).
//   Uses hardware-accelerated atan2f/asinf for speed on modern platforms.
//
static vec2_t
sphere_to_uv(vec3_t point)
{
  vec2_t uv;

  // Normalize to unit sphere (ensures |point| = 1 for accurate spherical
  // coords)
  point = vec3_normalize(point);

  // Convert Cartesian (x,y,z) to spherical (θ, φ)
  float theta = atan2f(point.x, point.z); // Longitude: azimuth around Y axis
  float phi   = asinf(point.y);           // Latitude: elevation from XZ plane

  // Map spherical coordinates to UV texture space [0, 1]
  uv.u = theta / (2.0f * M_PI) + 0.5f; // Horizontal: [-π,π] → [0,1]
  uv.v = phi / M_PI + 0.5f;            // Vertical: [-π/2,π/2] → [0,1]

  return uv;
}

// ════════════════════════════════════════════════════════════
// FIXED-POINT MATH UTILITIES (16.16 format)
// ════════════════════════════════════════════════════════════

// 16.16 fixed-point format: 16 bits integer, 16 bits fractional
// Range: -32768.0 to +32767.99998 (±2^15)
// Precision: 1/65536 ≈ 0.000015
//
// This provides excellent precision for graphics operations while being
// much faster than floating-point on WASM, especially for multiplication
// and interpolation operations like bilinear filtering.

typedef int32_t fixed16_t;

// Fixed-point constants
#define FIXED_SHIFT 16               // 16 fractional bits
#define FIXED_ONE (1 << FIXED_SHIFT) // 1.0 in fixed-point (65536)
#define FIXED_HALF (FIXED_ONE >> 1)  // 0.5 in fixed-point (32768)

// Conversion macros
#define F2FX(f) ((fixed16_t)((f) * FIXED_ONE))    // float → fixed
#define FX2F(i) ((float)(i) / (float)FIXED_ONE)   // fixed → float
#define I2FX(i) ((fixed16_t)((i) << FIXED_SHIFT)) // int → fixed

// Arithmetic operations
#define FX_MUL(a, b) ((fixed16_t)(((int64_t)(a) * (b)) >> FIXED_SHIFT))
#define FX_DIV(a, b) ((fixed16_t)(((int64_t)(a) << FIXED_SHIFT) / (b)))

// Fast integer extraction
#define FX2I(i) ((int)((i) >> FIXED_SHIFT)) // fixed → int (floor)

// ════════════════════════════════════════════════════════════
// TEXTURE UTILITIES
// ════════════════════════════════════════════════════════════

// Texture structure
typedef struct
{
  uint8_t *data; // RGBA pixel data
  int width;
  int height;
} texture_t;

// Load PNG texture from file
static texture_t *
texture_load_png(const char *filepath)
{
  texture_t *tex = (texture_t *)malloc(sizeof(texture_t));
  if (!tex)
    return NULL;

  int channels;

  tex->data = stbi_load(filepath, &tex->width, &tex->height, &channels, 4);

  if (!tex->data)
    {
      LOG_ERROR("stbi_load failed for: %s (reason: %s)", filepath,
                stbi_failure_reason());
      free(tex);
      return NULL;
    }

  LOG_INFO("Loaded texture: %s (%dx%d, %d channels)", filepath, tex->width,
           tex->height, channels);

  return tex;
}

// Free texture
static void
texture_free(texture_t *tex)
{
  if (tex)
    {
      if (tex->data)
        stbi_image_free(tex->data);
      free(tex);
    }
}

// Sample texture with bilinear filtering (UV in 0-1 range)
//
// Uses floating-point math for highest quality. Benchmarking showed float
// is actually 3.8% FASTER than fixed-point (179 μs vs 186 μs) due to
// modern JIT optimizations in Wasmtime/Cranelift.
static uint32_t
texture_sample(const texture_t *tex, float u, float v)
{
  if (!tex || !tex->data)
    return 0xFF000000; // Black with full alpha

  // Wrap UV coordinates
  u = u - floorf(u);
  v = v - floorf(v);

  // Convert to pixel coordinates (use full width/height for seamless wrapping)
  float fx = u * tex->width;
  float fy = v * tex->height;

  // Wrap fx/fy before extracting integer part (handles exactly 1.0 case)
  fx = fmodf(fx, (float)tex->width);
  fy = fmodf(fy, (float)tex->height);

  int x0 = (int)fx;
  int y0 = (int)fy;
  int x1 = (x0 + 1) % tex->width;
  int y1 = (y0 + 1) % tex->height;

  float tx = fx - x0;
  float ty = fy - y0;

  // Get 4 neighbor pixels
  uint8_t *p00 = &tex->data[(y0 * tex->width + x0) * 4];
  uint8_t *p10 = &tex->data[(y0 * tex->width + x1) * 4];
  uint8_t *p01 = &tex->data[(y1 * tex->width + x0) * 4];
  uint8_t *p11 = &tex->data[(y1 * tex->width + x1) * 4];

  // Bilinear interpolation
  uint8_t r = (uint8_t)((1 - tx) * (1 - ty) * p00[0] + tx * (1 - ty) * p10[0]
                        + (1 - tx) * ty * p01[0] + tx * ty * p11[0]);
  uint8_t g = (uint8_t)((1 - tx) * (1 - ty) * p00[1] + tx * (1 - ty) * p10[1]
                        + (1 - tx) * ty * p01[1] + tx * ty * p11[1]);
  uint8_t b = (uint8_t)((1 - tx) * (1 - ty) * p00[2] + tx * (1 - ty) * p10[2]
                        + (1 - tx) * ty * p01[2] + tx * ty * p11[2]);
  uint8_t a = (uint8_t)((1 - tx) * (1 - ty) * p00[3] + tx * (1 - ty) * p10[3]
                        + (1 - tx) * ty * p01[3] + tx * ty * p11[3]);

  // Assemble RGBA color (0xAABBGGRR format)
  return (a << 24) | (b << 16) | (g << 8) | r;
}

// ════════════════════════════════════════════════════════════
// NAV BALL PRECOMPUTATION (LOOKUP TABLE)
// ════════════════════════════════════════════════════════════

// Lookup table entry for nav ball rendering optimization
typedef struct
{
  vec3_t sphere_point; // Pre-computed normalized 3D point on sphere
  bool valid;          // Is this pixel inside the sphere?
} navball_lut_entry_t;

// Lookup table structure per nav ball instance
typedef struct
{
  navball_lut_entry_t *entries; // Array of LUT entries
  int size;                     // Nav ball size (width/height)
  int total_pixels;             // size * size
  float radius;                 // Sphere radius
} navball_lut_t;

// Initialize lookup table for nav ball rendering
// Pre-computes sphere geometry to eliminate per-frame calculations
static navball_lut_t *
navball_lut_create(int size)
{
  navball_lut_t *lut = (navball_lut_t *)malloc(sizeof(navball_lut_t));
  if (!lut)
    return NULL;

  lut->size         = size;
  lut->total_pixels = size * size;
  lut->radius       = size / 2.0f;

  // Allocate LUT entries
  lut->entries = (navball_lut_entry_t *)calloc(lut->total_pixels,
                                               sizeof(navball_lut_entry_t));
  if (!lut->entries)
    {
      free(lut);
      return NULL;
    }

  // Pre-compute sphere geometry for each pixel
  LOG_INFO("Nav ball: Pre-computing LUT for %dx%d (%d pixels)...", size, size,
           lut->total_pixels);

#ifndef NDEBUG
  int pixels_inside = 0;
#endif
  for (int y = 0; y < size; y++)
    {
      for (int x = 0; x < size; x++)
        {
          int idx = y * size + x;

          // Convert screen coordinates to sphere space (centered at origin)
          float sx = x - lut->radius;
          float sy = y - lut->radius;

          // Check if point is inside sphere
          float dist_sq   = sx * sx + sy * sy;
          float radius_sq = lut->radius * lut->radius;

          if (dist_sq <= radius_sq)
            {
              // Calculate Z coordinate on sphere surface
              float sz = sqrtf(radius_sq - dist_sq);

              // Create and normalize 3D point on sphere surface
              vec3_t point = vec3_new(sx, sy, sz);
              point        = vec3_normalize(point);

              // Store pre-computed normalized point
              lut->entries[idx].sphere_point = point;
              lut->entries[idx].valid        = true;
#ifndef NDEBUG
              pixels_inside++;
#endif
            }
          else
            {
              lut->entries[idx].valid = false;
            }
        }
    }

  LOG_INFO("Nav ball: LUT created - %d pixels inside sphere (%.1f%%)",
           pixels_inside, (pixels_inside * 100.0f) / lut->total_pixels);

  return lut;
}

// Free lookup table
static void
navball_lut_free(navball_lut_t *lut)
{
  if (lut)
    {
      if (lut->entries)
        free(lut->entries);
      free(lut);
    }
}

// ════════════════════════════════════════════════════════════
// NAV BALL SKIN MAPPING
// ════════════════════════════════════════════════════════════

const char *
navball_skin_to_filename(navball_skin_t skin)
{
  switch (skin)
    {
    case NAVBALL_SKIN_STOCK:
      return "stock.png";
    case NAVBALL_SKIN_STOCK_IVA:
      return "stock-iva.png";
    case NAVBALL_SKIN_5TH_HORSEMAN_V2:
      return "5thHorseman_v2-navball.png";
    case NAVBALL_SKIN_5TH_HORSEMAN_BLACK:
      return "5thHorseman-navball_blackgrey_DIF.png";
    case NAVBALL_SKIN_5TH_HORSEMAN_BROWN:
      return "5thHorseman-navball_brownblue_DIF.png";
    case NAVBALL_SKIN_JAFO:
      return "JAFO.png";
    case NAVBALL_SKIN_KBOB_V2:
      return "kBob_v2.2.png";
    case NAVBALL_SKIN_ORDINARY_KERMAN:
      return "OrdinaryKerman.png";
    case NAVBALL_SKIN_TREKKY:
      return "Trekky0623_DIF.png";
    case NAVBALL_SKIN_APOLLO:
      return "tooRelic_Apollo.png";
    case NAVBALL_SKIN_WHITE_OWL:
      return "White_Owl.png";
    case NAVBALL_SKIN_ZASNOLD:
      return "Zasnold_DIF.png";
    case NAVBALL_SKIN_FALCONB:
      return "FalconB.png";
    default:
      return "stock.png";
    }
}

navball_skin_t
navball_skin_from_string(const char *name)
{
  if (!name)
    return NAVBALL_SKIN_STOCK;

  if (strcmp(name, "stock_iva") == 0)
    return NAVBALL_SKIN_STOCK_IVA;
  if (strcmp(name, "5th_horseman_v2") == 0)
    return NAVBALL_SKIN_5TH_HORSEMAN_V2;
  if (strcmp(name, "5th_horseman_black") == 0)
    return NAVBALL_SKIN_5TH_HORSEMAN_BLACK;
  if (strcmp(name, "5th_horseman_brown") == 0)
    return NAVBALL_SKIN_5TH_HORSEMAN_BROWN;
  if (strcmp(name, "jafo") == 0)
    return NAVBALL_SKIN_JAFO;
  if (strcmp(name, "kbob") == 0)
    return NAVBALL_SKIN_KBOB_V2;
  if (strcmp(name, "ordinary_kerman") == 0)
    return NAVBALL_SKIN_ORDINARY_KERMAN;
  if (strcmp(name, "trekky") == 0)
    return NAVBALL_SKIN_TREKKY;
  if (strcmp(name, "apollo") == 0)
    return NAVBALL_SKIN_APOLLO;
  if (strcmp(name, "white_owl") == 0)
    return NAVBALL_SKIN_WHITE_OWL;
  if (strcmp(name, "zasnold") == 0)
    return NAVBALL_SKIN_ZASNOLD;
  if (strcmp(name, "falconb") == 0)
    return NAVBALL_SKIN_FALCONB;

  return NAVBALL_SKIN_STOCK; // Default
}

// ════════════════════════════════════════════════════════════
// NAV BALL WIDGET IMPLEMENTATION
// ════════════════════════════════════════════════════════════

bool
navball_init(osd_context_t *ctx, const navball_config_t *config)
{
  if (!ctx || !config)
    {
      LOG_ERROR("Nav ball init called with NULL parameters");
      return false;
    }

  // Store configuration
  ctx->navball_enabled           = config->enabled;
  ctx->navball_x                 = config->position_x;
  ctx->navball_y                 = config->position_y;
  ctx->navball_size              = config->size;
  ctx->navball_skin              = config->skin;
  ctx->navball_show_level_marker = config->show_level_marker;

  // Store center indicator configuration
  ctx->navball_show_center_indicator  = config->show_center_indicator;
  ctx->navball_center_indicator_scale = config->center_indicator_scale;

  if (!config->enabled)
    {
      LOG_INFO("Nav ball disabled in config");
      return true; // Not an error, just disabled
    }

  // Load skin texture
  const char *skin_filename = navball_skin_to_filename(config->skin);
  char skin_path[512];
  snprintf(skin_path, sizeof(skin_path), "resources/navball_skins/%s",
           skin_filename);

  ctx->navball_texture = (void *)texture_load_png(skin_path);

  if (!ctx->navball_texture)
    {
      LOG_ERROR("Failed to load nav ball skin: %s", skin_path);
      ctx->navball_enabled = false;
      return false;
    }

  // Create lookup table for precomputed sphere geometry
  ctx->navball_lut = (void *)navball_lut_create(config->size);

  if (!ctx->navball_lut)
    {
      LOG_ERROR("Failed to create nav ball LUT");
      texture_free((texture_t *)ctx->navball_texture);
      ctx->navball_texture = NULL;
      ctx->navball_enabled = false;
      return false;
    }

  // Load center indicator SVG if enabled
  if (config->show_center_indicator
      && config->center_indicator_svg_path[0] != '\0')
    {
      if (svg_load(&ctx->navball_center_indicator_svg,
                   config->center_indicator_svg_path))
        {
          LOG_INFO("Nav ball center indicator loaded: %s",
                   config->center_indicator_svg_path);
        }
      else
        {
          LOG_WARN("Failed to load center indicator SVG: %s",
                   config->center_indicator_svg_path);
          ctx->navball_show_center_indicator = false;
        }
    }

  // Load celestial indicator SVGs if enabled
  if (ctx->celestial_enabled)
    {
      bool all_loaded = true;

      // Load sun front SVG
      if (ctx->config.celestial_indicators.sun_front_svg_path[0] != '\0')
        {
          if (!svg_load(&ctx->celestial_sun_front_svg,
                        ctx->config.celestial_indicators.sun_front_svg_path))
            {
              LOG_WARN("Failed to load sun front SVG: %s",
                       ctx->config.celestial_indicators.sun_front_svg_path);
              all_loaded = false;
            }
        }

      // Load sun back SVG
      if (ctx->config.celestial_indicators.sun_back_svg_path[0] != '\0')
        {
          if (!svg_load(&ctx->celestial_sun_back_svg,
                        ctx->config.celestial_indicators.sun_back_svg_path))
            {
              LOG_WARN("Failed to load sun back SVG: %s",
                       ctx->config.celestial_indicators.sun_back_svg_path);
              all_loaded = false;
            }
        }

      // Load moon front SVG
      if (ctx->config.celestial_indicators.moon_front_svg_path[0] != '\0')
        {
          if (!svg_load(&ctx->celestial_moon_front_svg,
                        ctx->config.celestial_indicators.moon_front_svg_path))
            {
              LOG_WARN("Failed to load moon front SVG: %s",
                       ctx->config.celestial_indicators.moon_front_svg_path);
              all_loaded = false;
            }
        }

      // Load moon back SVG
      if (ctx->config.celestial_indicators.moon_back_svg_path[0] != '\0')
        {
          if (!svg_load(&ctx->celestial_moon_back_svg,
                        ctx->config.celestial_indicators.moon_back_svg_path))
            {
              LOG_WARN("Failed to load moon back SVG: %s",
                       ctx->config.celestial_indicators.moon_back_svg_path);
              all_loaded = false;
            }
        }

      if (all_loaded)
        {
          LOG_INFO("Celestial indicators loaded (sun=%d, moon=%d)",
                   ctx->celestial_show_sun, ctx->celestial_show_moon);
        }
      else
        {
          LOG_WARN("Some celestial SVGs failed to load, disabling feature");
          ctx->celestial_enabled = false;
        }
    }

  LOG_INFO("Nav ball initialized: %s at (%d,%d) size=%d", skin_filename,
           config->position_x, config->position_y, config->size);
  return true;
}

// ════════════════════════════════════════════════════════════
// NAVBALL RENDERING PIPELINE
// ════════════════════════════════════════════════════════════
//
// This function renders a 3D rotating sphere (navball) that displays the
// platform's orientation. It's the heart of the attitude indicator system.
//
// RENDERING PIPELINE (per-pixel):
//   1. Get precomputed sphere point from LUT (eliminates sqrt + normalize)
//   2. Apply rotation matrix to sphere point (3×3 matrix-vector multiply)
//   3. Convert rotated point to UV coordinates (atan2 + asin)
//   4. Sample texture at UV with bilinear filtering (4 texture fetches + lerp)
//   5. Apply lighting (simple diffuse: N·L)
//   6. Alpha blend to framebuffer (Porter-Duff over compositing)
//
// PERFORMANCE OPTIMIZATIONS:
//   - LUT precomputation: ~51,000 entries cache sphere geometry (saves 70k sqrt
//   calls/frame)
//   - Bilinear filtering: Fixed-point 16.16 math for fast interpolation
//   - Early rejection: LUT marks invalid pixels (outside circle) to skip
//   processing
//
// CUSTOMIZATION POINTS for developers:
//   - Change skin: Set ctx->navball_texture to different texture
//   - Change size: Set ctx->navball_size (automatically rebuilds LUT)
//   - Change position: Set ctx->navball_x, ctx->navball_y
//   - Disable level marker: Set ctx->navball_show_level_marker = false
//
bool
navball_render(osd_context_t *ctx, osd_state_t *pb_state)
{
  // Guard clauses: verify navball is initialized and ready to render
  if (!ctx || !ctx->navball_enabled || !ctx->navball_texture
      || !ctx->navball_lut)
    {
      return false;
    }

  texture_t *skin = (texture_t *)ctx->navball_texture;

  // ════════════════════════════════════════════════════════════
  // COMPASS DATA → ROTATION MATRIX (QUATERNION-BASED)
  // ════════════════════════════════════════════════════════════
  //
  // The navball displays orientation by rotating the sphere texture to match
  // the platform's attitude. Compass data provides 3 angles (Euler angles):
  //
  //   - Azimuth (Yaw):    0-360° heading (0°=North, 90°=East, 180°=South,
  //   270°=West)
  //   - Elevation (Pitch): -90° to +90° nose angle (positive = nose up)
  //   - Bank (Roll):      -180° to +180° wing angle (positive = right wing
  //   down)
  //
  // ROTATION ORDER (YXZ intrinsic):
  //   1. Pitch (elevation) - rotates around lateral axis
  //   2. Roll (bank) - rotates around longitudinal axis
  //   3. Yaw (azimuth) - rotates around vertical axis
  //
  //   This order ensures proper axis independence and prevents gimbal lock
  //   in the -45° to +45° pitch range.
  //
  // QUATERNION CONVERSION:
  //   Euler angles → Quaternion → Rotation matrix
  //   This avoids gimbal lock and provides smooth rotation at all attitudes.
  //
  // ROTATION INVERSION:
  //   The navball is a fixed-camera view of a rotating sphere.
  //   - Pitch (elevation): Inverted - platform up → sphere down (sky visible
  //   at bottom)
  //   - Yaw (azimuth): Inverted - platform rotates CW → sphere appears to
  //   rotate CCW
  //   - Roll (bank): Inverted - platform rolls right → sphere appears to roll
  //   left
  //
  float azimuth   = 0.0f; // Yaw (heading around vertical axis)
  float elevation = 0.0f; // Pitch (nose up/down)
  float bank      = 0.0f; // Roll (wing tilt)

  if (pb_state->has_actual_space_time)
    {
      azimuth   = (float)pb_state->actual_space_time.azimuth;
      elevation = (float)pb_state->actual_space_time.elevation;
      bank      = (float)pb_state->actual_space_time.bank;
    }

  // Convert degrees to radians (remove negations to fix direction inversions)
  float pitch_rad = DEG_TO_RAD(elevation); // Platform up → sphere up
  float roll_rad  = DEG_TO_RAD(azimuth);   // SWAPPED: azimuth → roll axis
  float yaw_rad   = DEG_TO_RAD(bank);      // SWAPPED: bank → yaw axis

  // Build rotation matrix using cglm library (gimbal-lock-free quaternion
  // conversion) YXZ intrinsic order: pitch first (angles[0]), then roll
  // (angles[1]), then yaw (angles[2])
  vec3 angles = { pitch_rad, roll_rad, yaw_rad }; // cglm vec3 is float[3]
  versor q;      // cglm versor is float[4] (quaternion)
  mat4 cglm_mat; // cglm mat4 is float[16]

  glm_euler_yxz_quat(angles, q); // Convert YXZ euler angles to quaternion
  glm_quat_mat4(q, cglm_mat);    // Convert quaternion to 4×4 matrix

  // Copy cglm mat4 to our local mat4_t struct
  mat4_t rotation;
  memcpy(rotation.m, cglm_mat, sizeof(float) * 16);

  // Framebuffer setup
  framebuffer_t fb;
  framebuffer_init(&fb, ctx->framebuffer, ctx->width, ctx->height);

  // Get precomputed lookup table
  navball_lut_t *lut = (navball_lut_t *)ctx->navball_lut;

  // Pre-compute lighting direction (normalize once, not per pixel)
  vec3_t light_dir = vec3_normalize(vec3_new(0.3f, 0.3f, 1.0f));

  for (int y = 0; y < ctx->navball_size; y++)
    {
      for (int x = 0; x < ctx->navball_size; x++)
        {
          // Get precomputed LUT entry
          int idx                    = y * lut->size + x;
          navball_lut_entry_t *entry = &lut->entries[idx];

          // Skip pixels outside sphere (precomputed validity check)
          if (!entry->valid)
            continue;

          // Use precomputed normalized 3D point (eliminates sqrtf + normalize!)
          vec3_t point = entry->sphere_point;

          // Apply rotation
          vec3_t rotated = mat4_mul_vec3(rotation, point);

          // Convert to UV coordinates
          vec2_t uv = sphere_to_uv(rotated);

          // Sample skin texture with floating-point bilinear filtering
          uint32_t color = texture_sample(skin, uv.u, uv.v);

          // Apply simple lighting for depth perception
          // Note: Using precomputed point as normal (already normalized)
          float ndotl = vec3_dot(point, light_dir);
          ndotl       = (ndotl < 0) ? 0 : ndotl;

          float lighting = 0.4f + 0.6f * ndotl; // Ambient + diffuse

          // Apply lighting (extract RGBA channels: 0xAABBGGRR)
          uint8_t r = (uint8_t)((color & 0xFF) * lighting);
          uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * lighting);
          uint8_t b = (uint8_t)(((color >> 16) & 0xFF) * lighting);
          uint8_t a = (color >> 24) & 0xFF;

          // Blend to framebuffer
          int screen_x = ctx->navball_x + x;
          int screen_y = ctx->navball_y + y;

          if (screen_x >= 0 && screen_x < (int)ctx->width && screen_y >= 0
              && screen_y < (int)ctx->height)
            {
              // Assemble RGBA color (0xAABBGGRR format)
              uint32_t lit_color = (a << 24) | (b << 16) | (g << 8) | r;
              framebuffer_blend_pixel(&fb, screen_x, screen_y, lit_color);
            }
        }
    }

  // ════════════════════════════════════════════════════════════
  // CENTER INDICATOR OVERLAY
  // ════════════════════════════════════════════════════════════
  //
  // Render SVG center indicator overlay (circle + dot) at navball center.
  // This helps visualize the camera pointing direction.

  if (ctx->navball_show_center_indicator
      && ctx->navball_center_indicator_svg.image)
    {
      // Calculate indicator size based on scale
      int indicator_size
        = (int)(ctx->navball_size * ctx->navball_center_indicator_scale);

      // Center the indicator on the navball
      int indicator_x
        = ctx->navball_x + (ctx->navball_size - indicator_size) / 2;
      int indicator_y
        = ctx->navball_y + (ctx->navball_size - indicator_size) / 2;

      // Rasterize and render the SVG indicator
      svg_render(&fb, &ctx->navball_center_indicator_svg, indicator_x,
                 indicator_y, indicator_size, indicator_size);
    }

  // ════════════════════════════════════════════════════════════
  // CELESTIAL INDICATORS (SUN AND MOON)
  // ════════════════════════════════════════════════════════════
  //
  // Render sun and moon position indicators on the navball using real-time
  // astronomical calculations. Shows celestial body positions relative to
  // the platform's orientation.

  if (ctx->celestial_enabled && pb_state->has_actual_space_time)
    {
      // Extract GPS location and timestamp from protobuf
      observer_location_t observer;
      observer.latitude  = pb_state->actual_space_time.latitude;
      observer.longitude = pb_state->actual_space_time.longitude;
      observer.altitude  = pb_state->actual_space_time.altitude;

      int64_t timestamp = pb_state->actual_space_time.timestamp;

      // Calculate sun and moon positions
      celestial_positions_t positions
        = celestial_calculate(timestamp, observer);

      // Get platform orientation for coordinate transformation
      double platform_azimuth   = pb_state->actual_space_time.azimuth;
      double platform_elevation = pb_state->actual_space_time.elevation;
      double platform_bank      = pb_state->actual_space_time.bank;

      // Calculate navball geometry
      int navball_center_x = ctx->navball_x + ctx->navball_size / 2;
      int navball_center_y = ctx->navball_y + ctx->navball_size / 2;
      int navball_radius   = ctx->navball_size / 2;

      // Default indicator size (52% of navball size for better visibility)
      int indicator_size
        = (int)(ctx->navball_size * 0.52f * ctx->celestial_indicator_scale);

      // ──────────────────────────────────────────────────────────
      // RENDER SUN INDICATOR
      // ──────────────────────────────────────────────────────────

      if (ctx->celestial_show_sun && positions.sun.valid)
        {
          // Check visibility threshold (e.g., only show if above -5°)
          if (positions.sun.altitude >= ctx->celestial_visibility_threshold)
            {
              // Convert celestial coordinates to navball screen position
              int sun_x, sun_y;
              bool is_front = celestial_to_navball_coords(
                positions.sun.azimuth, positions.sun.altitude, platform_azimuth,
                platform_elevation, platform_bank, navball_center_x,
                navball_center_y, navball_radius, &sun_x, &sun_y);

              // Select appropriate SVG based on visibility
              svg_resource_t *sun_svg = is_front ? &ctx->celestial_sun_front_svg
                                                 : &ctx->celestial_sun_back_svg;

              // Behind indicators: smaller size (70%) and reduced opacity (50%)
              int render_size
                = is_front ? indicator_size : (int)(indicator_size * 0.7f);
              float render_alpha = is_front ? 1.0f : 0.5f;

              // Render sun indicator centered at calculated position
              if (sun_svg->image)
                {
                  int sun_render_x = sun_x - render_size / 2;
                  int sun_render_y = sun_y - render_size / 2;

                  svg_render_with_alpha(&fb, sun_svg, sun_render_x,
                                        sun_render_y, render_size, render_size,
                                        render_alpha);
                }
            }
        }

      // ──────────────────────────────────────────────────────────
      // RENDER MOON INDICATOR
      // ──────────────────────────────────────────────────────────

      if (ctx->celestial_show_moon && positions.moon.valid)
        {
          // Check visibility threshold
          if (positions.moon.altitude >= ctx->celestial_visibility_threshold)
            {
              // Convert celestial coordinates to navball screen position
              int moon_x, moon_y;
              bool is_front = celestial_to_navball_coords(
                positions.moon.azimuth, positions.moon.altitude,
                platform_azimuth, platform_elevation, platform_bank,
                navball_center_x, navball_center_y, navball_radius, &moon_x,
                &moon_y);

              // Select appropriate SVG based on visibility
              svg_resource_t *moon_svg = is_front
                                           ? &ctx->celestial_moon_front_svg
                                           : &ctx->celestial_moon_back_svg;

              // Behind indicators: smaller size (70%) and reduced opacity (50%)
              int render_size
                = is_front ? indicator_size : (int)(indicator_size * 0.7f);
              float render_alpha = is_front ? 1.0f : 0.5f;

              // Render moon indicator centered at calculated position
              if (moon_svg->image)
                {
                  int moon_render_x = moon_x - render_size / 2;
                  int moon_render_y = moon_y - render_size / 2;

                  svg_render_with_alpha(&fb, moon_svg, moon_render_x,
                                        moon_render_y, render_size, render_size,
                                        render_alpha);
                }
            }
        }
    }

  // ════════════════════════════════════════════════════════════
  // LEVEL MARKER (HORIZON LINE)
  // ════════════════════════════════════════════════════════════
  //
  // Draw a horizontal line across the center of the navball to indicate
  // level/horizon. This helps pilots quickly identify level flight attitude.

  if (ctx->navball_show_level_marker)
    {
      int center_y          = ctx->navball_y + ctx->navball_size / 2;
      int line_length       = ctx->navball_size;
      uint32_t marker_color = 0xFFFFFFFF; // White with full alpha

      // Draw horizontal line across navball center
      for (int x = 0; x < line_length; x++)
        {
          int screen_x = ctx->navball_x + x;

          // Only draw if pixel is inside the circle (check LUT validity)
          int lut_x = x;
          int lut_y = ctx->navball_size / 2;
          int idx   = lut_y * lut->size + lut_x;

          if (idx >= 0 && idx < lut->total_pixels && lut->entries[idx].valid)
            {
              framebuffer_blend_pixel(&fb, screen_x, center_y, marker_color);
            }
        }
    }

  return true;
}

void
navball_cleanup(osd_context_t *ctx)
{
  if (!ctx)
    return;

  // Free texture
  if (ctx->navball_texture)
    {
      texture_free((texture_t *)ctx->navball_texture);
      ctx->navball_texture = NULL;
    }

  // Free lookup table
  if (ctx->navball_lut)
    {
      navball_lut_free((navball_lut_t *)ctx->navball_lut);
      ctx->navball_lut = NULL;
    }

  // Free center indicator SVG
  svg_free(&ctx->navball_center_indicator_svg);

  // Free celestial indicator SVGs
  svg_free(&ctx->celestial_sun_front_svg);
  svg_free(&ctx->celestial_sun_back_svg);
  svg_free(&ctx->celestial_moon_front_svg);
  svg_free(&ctx->celestial_moon_back_svg);
}
