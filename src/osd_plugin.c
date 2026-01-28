#include "osd_plugin.h"

// Standard C libraries (must be first for STB)
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Core modules
#include "core/framebuffer.h"

// New modular rendering system
#include "rendering/blending.h"
#include "rendering/primitives.h"
#include "rendering/text.h"

// Resource management
#include "resources/font.h"
#include "resources/svg.h"

// Widgets
#include "widgets/crosshair.h"
#include "widgets/navball.h"
#include "widgets/timestamp.h"
#include "widgets/variant_info.h"

// Configuration
#include "config_json.h"

// Utilities
#include "utils/logging.h"
#include "utils/math.h"
#include "utils/resource_lookup.h"

// Note: stb_truetype and nanosvg implementations are in resource modules
// (font.c and svg.c). We include headers here for rendering functions.
// Define math functions as builtins to avoid WASI SDK strict mode issues
#ifndef isnan
#define isnan(x) __builtin_isnan(x)
#endif
#ifndef fabs
#define fabs(x) __builtin_fabs(x)
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-function-declaration"

#include "nanosvg.h"
#include "nanosvgrast.h"
#include "stb_truetype.h"

#pragma clang diagnostic pop

// Protocol buffer support
#include "jon_shared_data.pb.h"
#include "jon_shared_data_types.pb.h"
#include "pb_decode.h"

// Opaque payload protos
// Note: First sync with `make proto` if this include fails
#include "opaque/osd_client_metadata.pb.h"

// UUID for OsdClientMetadata opaque payload
#define OSD_CLIENT_METADATA_UUID "01941b00-0000-7000-8000-000000000001"

// Mini XML parser (will include if available)
// #include <mxml.h>

// ════════════════════════════════════════════════════════════
// GLOBAL CONTEXT
// ════════════════════════════════════════════════════════════

static osd_context_t g_osd_ctx             = { 0 };
static uint32_t g_framebuffer[1920 * 1080] = { 0 }; // Max size

// ════════════════════════════════════════════════════════════
// RENDERING HELPERS
// ════════════════════════════════════════════════════════════
//
// ctx_to_framebuffer() moved to core/context_helpers.h
// Crosshair rendering moved to widgets/crosshair.c
// Text rendering: widgets use rendering/text.h directly

// ════════════════════════════════════════════════════════════
// CONFIGURATION LOADING (JSON)
// ════════════════════════════════════════════════════════════

bool
load_config_xml(osd_context_t *ctx, const char *path)
{
  LOG_INFO("Loading config from: %s", path);

  // Parse JSON configuration
  if (!config_parse_json(&ctx->config, path))
    {
      LOG_WARN("Failed to parse JSON config, using defaults");
      return false;
    }

  // Font paths are now resolved from JSON config in config_json.c
  // Each widget (timestamp, speed_indicators, variant_info) has its own font
  // setting

  LOG_INFO("Config loaded successfully");
  return true;
}

// Font and SVG loading now handled by resource modules (resources/font.c,
// resources/svg.c) See font_load() and svg_load() for implementation

// ════════════════════════════════════════════════════════════
// OPAQUE PAYLOAD PARSING
// ════════════════════════════════════════════════════════════

// Struct to hold callback context for opaque payload parsing
typedef struct
{
  osd_context_t *ctx;
  char uuid_buffer[64];
  uint8_t payload_buffer[128];
  size_t payload_size;
  bool uuid_matched;
} opaque_payload_ctx_t;

// Callback for type_uuid string field in JonOpaquePayload
static bool
opaque_uuid_decode_callback(pb_istream_t *stream,
                            const pb_field_t *field,
                            void **arg)
{
  opaque_payload_ctx_t *cb_ctx = (opaque_payload_ctx_t *)*arg;
  (void)field; // unused

  size_t len = stream->bytes_left;
  if (len >= sizeof(cb_ctx->uuid_buffer))
    {
      len = sizeof(cb_ctx->uuid_buffer) - 1;
    }

  if (!pb_read(stream, (pb_byte_t *)cb_ctx->uuid_buffer, len))
    {
      return false;
    }

  cb_ctx->uuid_buffer[len] = '\0';

  // Check if this UUID matches our OsdClientMetadata UUID
  cb_ctx->uuid_matched
    = (strcmp(cb_ctx->uuid_buffer, OSD_CLIENT_METADATA_UUID) == 0);

  return true;
}

// Callback for payload bytes field in JonOpaquePayload
static bool
opaque_payload_decode_callback(pb_istream_t *stream,
                               const pb_field_t *field,
                               void **arg)
{
  opaque_payload_ctx_t *cb_ctx = (opaque_payload_ctx_t *)*arg;
  (void)field; // unused

  cb_ctx->payload_size = stream->bytes_left;
  if (cb_ctx->payload_size > sizeof(cb_ctx->payload_buffer))
    {
      // Payload too large, skip with warning
      LOG_WARN("Oversized opaque payload (%zu > %zu max), skipping",
               cb_ctx->payload_size, sizeof(cb_ctx->payload_buffer));
      cb_ctx->payload_size = 0;
      return pb_read(stream, NULL, stream->bytes_left);
    }

  if (!pb_read(stream, cb_ctx->payload_buffer, cb_ctx->payload_size))
    {
      return false;
    }

  return true;
}

// Callback for opaque_payloads repeated field in JonGUIState
static bool
opaque_payloads_decode_callback(pb_istream_t *stream,
                                const pb_field_t *field,
                                void **arg)
{
  osd_context_t *ctx = (osd_context_t *)*arg;
  (void)field; // unused

  // Set up context for nested callbacks
  opaque_payload_ctx_t cb_ctx = { 0 };
  cb_ctx.ctx                  = ctx;
  cb_ctx.uuid_matched         = false;

  // Set up JonOpaquePayload with callbacks for its fields
  ser_JonOpaquePayload opaque_payload   = ser_JonOpaquePayload_init_zero;
  opaque_payload.type_uuid.funcs.decode = opaque_uuid_decode_callback;
  opaque_payload.type_uuid.arg          = &cb_ctx;
  opaque_payload.payload.funcs.decode   = opaque_payload_decode_callback;
  opaque_payload.payload.arg            = &cb_ctx;

  // Decode the JonOpaquePayload submessage
  if (!pb_decode(stream, ser_JonOpaquePayload_fields, &opaque_payload))
    {
      LOG_WARN("Failed to decode opaque payload");
      return false;
    }

  // If UUID matched, decode the payload as OsdClientMetadata
  if (cb_ctx.uuid_matched && cb_ctx.payload_size > 0)
    {
      ser_OsdClientMetadata client_metadata = ser_OsdClientMetadata_init_zero;
      pb_istream_t payload_stream
        = pb_istream_from_buffer(cb_ctx.payload_buffer, cb_ctx.payload_size);

      if (pb_decode(&payload_stream, ser_OsdClientMetadata_fields,
                    &client_metadata))
        {
          // Validate ranges to prevent overflow or division by zero
          if (client_metadata.canvas_width_px == 0
              || client_metadata.canvas_width_px > 40960
              || client_metadata.canvas_height_px == 0
              || client_metadata.canvas_height_px > 40960
              || client_metadata.device_pixel_ratio <= 0.0f
              || client_metadata.device_pixel_ratio > 10.0f
              || client_metadata.device_pixel_ratio
                   != client_metadata.device_pixel_ratio)
            {
              LOG_WARN("Invalid OsdClientMetadata: w=%u h=%u dpr=%f",
                       client_metadata.canvas_width_px,
                       client_metadata.canvas_height_px,
                       client_metadata.device_pixel_ratio);
              return true; // Continue processing other payloads
            }

          // Store in context for variant_info to read
          ctx->client_metadata.canvas_width_px
            = client_metadata.canvas_width_px;
          ctx->client_metadata.canvas_height_px
            = client_metadata.canvas_height_px;
          ctx->client_metadata.device_pixel_ratio
            = client_metadata.device_pixel_ratio;
          ctx->client_metadata.osd_buffer_width
            = client_metadata.osd_buffer_width;
          ctx->client_metadata.osd_buffer_height
            = client_metadata.osd_buffer_height;

          // Video proxy bounds (NDC)
          ctx->client_metadata.video_proxy_ndc_x
            = client_metadata.video_proxy_ndc_x;
          ctx->client_metadata.video_proxy_ndc_y
            = client_metadata.video_proxy_ndc_y;
          ctx->client_metadata.video_proxy_ndc_width
            = client_metadata.video_proxy_ndc_width;
          ctx->client_metadata.video_proxy_ndc_height
            = client_metadata.video_proxy_ndc_height;

          // Scale factor
          ctx->client_metadata.scale_factor = client_metadata.scale_factor;

          // Theme info
          ctx->client_metadata.is_sharp_mode = client_metadata.is_sharp_mode;
          ctx->client_metadata.theme_hue     = client_metadata.theme_hue;
          ctx->client_metadata.theme_chroma  = client_metadata.theme_chroma;
          ctx->client_metadata.theme_lightness
            = client_metadata.theme_lightness;

          ctx->client_metadata.valid = true;

          LOG_DEBUG("Parsed OsdClientMetadata: canvas=%ux%u @%.2fx -> %ux%u, "
                    "proxy=(%.2f,%.2f,%.2f,%.2f) s:%.2f, "
                    "theme=%s H:%.0f C:%.2f L:%.0f",
                    client_metadata.canvas_width_px,
                    client_metadata.canvas_height_px,
                    client_metadata.device_pixel_ratio,
                    client_metadata.osd_buffer_width,
                    client_metadata.osd_buffer_height,
                    client_metadata.video_proxy_ndc_x,
                    client_metadata.video_proxy_ndc_y,
                    client_metadata.video_proxy_ndc_width,
                    client_metadata.video_proxy_ndc_height,
                    client_metadata.scale_factor,
                    client_metadata.is_sharp_mode ? "Sharp" : "Default",
                    client_metadata.theme_hue, client_metadata.theme_chroma,
                    client_metadata.theme_lightness);
        }
      else
        {
          LOG_WARN("Failed to decode OsdClientMetadata payload");
        }
    }

  return true;
}

// ════════════════════════════════════════════════════════════
// PROTOCOL BUFFER DECODING
// ════════════════════════════════════════════════════════════

bool
decode_proto_state(osd_context_t *ctx, ser_JonGUIState *pb_state)
{
  if (!ctx->proto_valid || ctx->proto_size == 0)
    {
      return false;
    }

  // Wire up callback for opaque_payloads to extract OsdClientMetadata
  pb_state->opaque_payloads.funcs.decode = opaque_payloads_decode_callback;
  pb_state->opaque_payloads.arg          = ctx;

  pb_istream_t stream
    = pb_istream_from_buffer(ctx->proto_buffer, ctx->proto_size);
  bool status = pb_decode(&stream, ser_JonGUIState_fields, pb_state);

  if (!status)
    {
      LOG_ERROR("Proto decode failed: %s", PB_GET_ERROR(&stream));
      return false;
    }

  return true;
}

// ════════════════════════════════════════════════════════════
// EXPORTED WASM FUNCTIONS
// ════════════════════════════════════════════════════════════

// Forward declare WASI libc initialization function
extern void __wasm_call_ctors(void);

// WASI Reactor Pattern: _initialize() is called by WASI runtime before any
// other functions This ensures libpreopen and WASI filesystem initialization
// happens even with -nostartfiles
__attribute__((export_name("_initialize"))) void
_initialize(void)
{
  // WASI runtime calls this to initialize the module
  // This is critical for file access - it sets up preopened directories

  // Call WASI libc constructors which initializes:
  // - __wasilibc_populate_preopens() - sets up preopened directory FD table
  // - __wasilibc_initialize_environ_eagerly() - environment variables
  __wasm_call_ctors();
}

// Get variant-specific config path based on compile-time defines
static const char *
get_config_path(void)
{
#if defined(OSD_MODE_LIVE) && defined(OSD_STREAM_DAY)
  return "build/resources/live_day_config.json";
#elif defined(OSD_MODE_LIVE) && defined(OSD_STREAM_THERMAL)
  return "build/resources/live_thermal_config.json";
#elif defined(OSD_MODE_RECORDING) && defined(OSD_STREAM_DAY)
  return "build/resources/recording_day_config.json";
#elif defined(OSD_MODE_RECORDING) && defined(OSD_STREAM_THERMAL)
  return "build/resources/recording_thermal_config.json";
#else
  return "build/resources/config.json"; // Fallback
#endif
}

/**
 * Initialize OSD system
 *
 * Initializes the OSD context, loads configuration, fonts, and resources.
 * Must be called before any other OSD functions.
 *
 * @return 0 on success, -1 on error
 */
__attribute__((visibility("default"))) int
wasm_osd_init(void)
{
  LOG_FUNC_INFO("Initializing OSD");

  // Initialize context with compile-time resolution
  g_osd_ctx.framebuffer  = g_framebuffer;
  g_osd_ctx.width        = CURRENT_FRAMEBUFFER_WIDTH;
  g_osd_ctx.height       = CURRENT_FRAMEBUFFER_HEIGHT;
  g_osd_ctx.needs_render = true;
  g_osd_ctx.frame_count  = 0;

  // Load variant-specific configuration
  const char *config_path = get_config_path();
  LOG_INFO("Loading config from: %s", config_path);
  if (!load_config_xml(&g_osd_ctx, config_path))
    {
      LOG_ERROR("Failed to load config");
      return -1;
    }

  // Load per-widget fonts
  // Each text-rendering widget has its own font for flexibility
  if (g_osd_ctx.config.timestamp.font_path[0])
    {
      LOG_INFO("Loading timestamp font: %s",
               g_osd_ctx.config.timestamp.font_path);
      if (!font_load(&g_osd_ctx.font_timestamp,
                     g_osd_ctx.config.timestamp.font_path))
        {
          LOG_ERROR("Timestamp font loading FAILED");
          return -1;
        }
    }
  else
    {
      LOG_ERROR("No timestamp font configured");
      return -1;
    }

  if (g_osd_ctx.config.speed_indicators.font_path[0])
    {
      LOG_INFO("Loading speed indicators font: %s",
               g_osd_ctx.config.speed_indicators.font_path);
      if (!font_load(&g_osd_ctx.font_speed_indicators,
                     g_osd_ctx.config.speed_indicators.font_path))
        {
          LOG_ERROR("Speed indicators font loading FAILED");
          return -1;
        }
    }
  else
    {
      LOG_ERROR("No speed indicators font configured");
      return -1;
    }

  if (g_osd_ctx.config.variant_info.font_path[0])
    {
      LOG_INFO("Loading variant info font: %s",
               g_osd_ctx.config.variant_info.font_path);
      if (!font_load(&g_osd_ctx.font_variant_info,
                     g_osd_ctx.config.variant_info.font_path))
        {
          LOG_ERROR("Variant info font loading FAILED");
          return -1;
        }
    }
  else
    {
      LOG_ERROR("No variant info font configured");
      return -1;
    }

  LOG_INFO("All fonts loaded successfully");

  // Copy celestial indicators configuration to context
  g_osd_ctx.celestial_enabled  = g_osd_ctx.config.celestial_indicators.enabled;
  g_osd_ctx.celestial_show_sun = g_osd_ctx.config.celestial_indicators.show_sun;
  g_osd_ctx.celestial_show_moon
    = g_osd_ctx.config.celestial_indicators.show_moon;
  g_osd_ctx.celestial_indicator_scale
    = g_osd_ctx.config.celestial_indicators.indicator_scale;
  g_osd_ctx.celestial_visibility_threshold
    = g_osd_ctx.config.celestial_indicators.visibility_threshold;

  // Initialize nav ball widget (REQUIRED - fail if initialization fails)
  // Note: navball_init() will load celestial SVGs if celestial_enabled is true
  LOG_INFO("Initializing nav ball widget...");
  if (!navball_init(&g_osd_ctx, &g_osd_ctx.config.navball))
    {
      LOG_ERROR("Nav ball initialization FAILED");
      return -1;
    }
  LOG_INFO("Nav ball initialized successfully");

  // Initialize proto buffer
  // update)
  g_osd_ctx.proto_size  = 0;
  g_osd_ctx.proto_valid = false;

  // Clear framebuffer
  memset(g_framebuffer, 0, sizeof(g_framebuffer));

  LOG_INFO("OSD initialized: %dx%d", g_osd_ctx.width, g_osd_ctx.height);
  return 0;
}

/**
 * Update OSD state from protobuf data
 *
 * Copies protobuf state data from host memory into WASM module.
 * This triggers a re-render on the next wasm_osd_render() call.
 *
 * @param state_ptr Pointer to protobuf data in host memory
 * @param state_size Size of protobuf data in bytes
 * @return 0 on success, -1 on error
 */
__attribute__((visibility("default"))) int
wasm_osd_update_state(uint32_t state_ptr, uint32_t state_size)
{
  if (state_size > sizeof(g_osd_ctx.proto_buffer))
    {
      LOG_ERROR("Proto too large: %u bytes (max %zu)", state_size,
                sizeof(g_osd_ctx.proto_buffer));
      return -1;
    }

  if (state_size == 0)
    {
      LOG_WARN("Empty state update");
      return -1;
    }

  // Copy proto bytes from host memory into our pre-allocated buffer
  memcpy(g_osd_ctx.proto_buffer, (void *)(uintptr_t)state_ptr, state_size);
  g_osd_ctx.proto_size   = state_size;
  g_osd_ctx.proto_valid  = true;
  g_osd_ctx.needs_render = true;
  g_osd_ctx.frame_count++;

  if ((g_osd_ctx.frame_count % 60) == 0)
    {
      LOG_INFO("State update #%d (proto size=%u bytes)", g_osd_ctx.frame_count,
               state_size);
    }

  return 0;
}

// ════════════════════════════════════════════════════════════
// RENDERING HELPERS
// ════════════════════════════════════════════════════════════

/**
 * Render all widgets and return whether anything changed
 *
 * @param proto_state Proto state (may be NULL if not decoded)
 * @return true if any widget rendered, false if nothing changed
 */
static bool
render_widgets(ser_JonGUIState *proto_state)
{
  bool changed = false;

  // Render crosshair (with or without speed indicators based on proto)
  changed |= crosshair_render(&g_osd_ctx, proto_state);

  // Render other widgets only if proto is available
  if (proto_state)
    {
      changed |= timestamp_render(&g_osd_ctx, proto_state);
      changed |= navball_render(&g_osd_ctx, proto_state);
    }

  // Variant info (needs proto for state time display)
  changed |= variant_info_render(&g_osd_ctx, proto_state);

  return changed;
}

// ════════════════════════════════════════════════════════════
// MAIN RENDERING FUNCTION
// ════════════════════════════════════════════════════════════

/**
 * Render OSD to framebuffer
 *
 * Renders all enabled widgets to the framebuffer. This function is idempotent -
 * if needs_render is false, it returns immediately without rendering.
 *
 * @return 1 if something was rendered, 0 if nothing changed or skipped
 */
__attribute__((visibility("default"))) int
wasm_osd_render(void)
{
  // Early return if nothing to render
  if (!g_osd_ctx.needs_render)
    {
      return 0;
    }

  // Clear framebuffer to transparent (alpha = 0)
  memset(g_framebuffer, 0, sizeof(g_framebuffer));

  // Decode proto state if available
  ser_JonGUIState pb_state = ser_JonGUIState_init_zero;
  ser_JonGUIState *pb_ptr  = NULL;

  if (g_osd_ctx.proto_valid && decode_proto_state(&g_osd_ctx, &pb_state))
    {
      pb_ptr = &pb_state; // Proto decoded successfully
    }

  // Render widgets and check if anything changed
  bool changed = render_widgets(pb_ptr);

  g_osd_ctx.needs_render = false;
  return changed ? 1 : 0;
}

/**
 * Get framebuffer pointer
 *
 * Returns a pointer to the OSD framebuffer in WASM linear memory.
 * The framebuffer contains RGBA pixel data for the entire OSD.
 *
 * @return Pointer to framebuffer (as uint32_t for WASM compatibility)
 */
__attribute__((visibility("default"))) uint32_t
wasm_osd_get_framebuffer(void)
{
  return (uint32_t)((uintptr_t)g_framebuffer);
}

/**
 * Destroy OSD system
 *
 * Frees all allocated resources (fonts, textures, LUTs, etc.) and resets
 * the OSD context. Should be called when the OSD is no longer needed.
 *
 * @return 0 on success
 */
__attribute__((visibility("default"))) int
wasm_osd_destroy(void)
{
  LOG_FUNC_INFO("Destroying OSD");

  // Free per-widget fonts
  font_free(&g_osd_ctx.font_timestamp);
  font_free(&g_osd_ctx.font_speed_indicators);
  font_free(&g_osd_ctx.font_variant_info);

  // Free SVG resources
  svg_free(&g_osd_ctx.cross_svg);
  svg_free(&g_osd_ctx.circle_svg);

  // Cleanup nav ball resources
  navball_cleanup(&g_osd_ctx);

  memset(&g_osd_ctx, 0, sizeof(g_osd_ctx));
  return 0;
}
