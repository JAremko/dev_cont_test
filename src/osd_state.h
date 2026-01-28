// OSD State Accessors
// Clean interface for widgets to access telemetry state
//
// ════════════════════════════════════════════════════════════
// WHY THIS EXISTS:
// Widgets need access to telemetry data (orientation, speeds, time, GPS).
// Rather than including raw protobuf headers, widgets use these accessors.
//
// BENEFITS:
// - Widgets don't need to know protobuf structure
// - Easy to test widgets with mock data
// - Can change underlying data format without touching widgets
// - Documents exactly what data each widget type needs
// ════════════════════════════════════════════════════════════

#ifndef OSD_STATE_H
#define OSD_STATE_H

#include <stdbool.h>
#include <stdint.h>

// Forward declare protobuf state (widgets don't need the full definition)
typedef struct _ser_JonGUIState osd_state_t;

// ════════════════════════════════════════════════════════════
// ORIENTATION DATA (for navball widget)
// ════════════════════════════════════════════════════════════

// Get platform orientation (compass)
// Returns true if data is valid
bool osd_state_get_orientation(const osd_state_t *state,
                               double *azimuth,   // 0-360 degrees
                               double *elevation, // -90 to +90 degrees
                               double *bank);     // -180 to +180 degrees

// ════════════════════════════════════════════════════════════
// SPEED DATA (for crosshair speed indicators)
// ════════════════════════════════════════════════════════════

// Get rotary speeds (normalized -1.0 to 1.0)
// Returns true if rotary is moving
bool osd_state_get_speeds(const osd_state_t *state,
                          double *azimuth_speed,   // -1.0 to 1.0
                          double *elevation_speed, // -1.0 to 1.0
                          bool *is_moving);

// ════════════════════════════════════════════════════════════
// CROSSHAIR OFFSET (for crosshair positioning)
// ════════════════════════════════════════════════════════════

// Get OSD offset for crosshair center
// offset_x, offset_y are in pixels from screen center
void osd_state_get_crosshair_offset(const osd_state_t *state,
                                    bool is_thermal_stream,
                                    int *offset_x,
                                    int *offset_y);

// ════════════════════════════════════════════════════════════
// TIME DATA (for timestamp widget)
// ════════════════════════════════════════════════════════════

// Get UTC timestamp
// Returns: Unix timestamp (seconds since epoch), or 0 if invalid
int64_t osd_state_get_timestamp(const osd_state_t *state);

// ════════════════════════════════════════════════════════════
// GPS DATA (for celestial calculations)
// ════════════════════════════════════════════════════════════

// GPS position data
typedef struct
{
  double latitude;   // -90 to +90 degrees
  double longitude;  // -180 to +180 degrees
  double altitude;   // meters above sea level
  int64_t timestamp; // Unix timestamp
  bool valid;
} osd_gps_position_t;

// Get GPS position from actual_space_time message
// Returns true if position data is valid
bool osd_state_get_gps(const osd_state_t *state, osd_gps_position_t *pos);

// ════════════════════════════════════════════════════════════
// CLIENT METADATA (canvas info from frontend, for debug overlay)
// ════════════════════════════════════════════════════════════

// Client-side canvas metadata
typedef struct
{
  // Canvas info
  uint32_t canvas_width_px;   // Physical canvas width (CSS width × DPR)
  uint32_t canvas_height_px;  // Physical canvas height (CSS height × DPR)
  float device_pixel_ratio;   // window.devicePixelRatio
  uint32_t osd_buffer_width;  // OSD framebuffer width (1920 or 900)
  uint32_t osd_buffer_height; // OSD framebuffer height (1080 or 720)

  // Video proxy bounds (NDC -1.0 to 1.0)
  float video_proxy_ndc_x;      // Quad X position
  float video_proxy_ndc_y;      // Quad Y position
  float video_proxy_ndc_width;  // Quad width
  float video_proxy_ndc_height; // Quad height

  // Scale factor (OSD buffer / proxy physical pixels)
  float scale_factor;

  // Theme info
  bool is_sharp_mode;    // true = high contrast, false = OKLCH
  float theme_hue;       // 0-360 degrees
  float theme_chroma;    // 0-1.0 saturation
  float theme_lightness; // 0-200 with HDR

  bool valid;
} osd_client_metadata_t;

// Get client metadata from opaque payload (if present)
// Returns true if metadata was found and parsed
// Note: This reads from context, not from state protobuf directly
typedef struct osd_context osd_context_t;
bool osd_state_get_client_metadata(const osd_context_t *ctx,
                                   osd_client_metadata_t *metadata);

// ════════════════════════════════════════════════════════════
// STATE TIMING DATA (for debug overlay)
// ════════════════════════════════════════════════════════════

// Get system monotonic time from state
// Returns: monotonic time in microseconds, or 0 if invalid
uint64_t osd_state_get_monotonic_time_us(const osd_state_t *state);

// Get frame monotonic capture time
// Returns: monotonic time in microseconds when frame was captured, or 0 if
// invalid
uint64_t osd_state_get_frame_monotonic_day_us(const osd_state_t *state);
uint64_t osd_state_get_frame_monotonic_heat_us(const osd_state_t *state);

#endif // OSD_STATE_H
