# Claude AI Assistant Context - Jettison WASM OSD

## Project Overview

C-to-WebAssembly OSD (On-Screen Display) plugin for Jettison. Compiles to WASM using WASI SDK and renders telemetry overlays including crosshair, navball, timestamp, and speed indicators.

**Key Features**:
- Docker-first development (Dev Container with full toolchain)
- Make-based build system (CLion compatible)
- 4 WASM variants (live/recording x day/thermal)
- 2 build modes (production/debug)
- Cryptographic package signing (RSA-SHA256)

## Build System

**IMPORTANT**: All builds require Docker and the devcontainer CLI. Run builds from the HOST using `./tools/devcontainer-build.sh` - this script handles the Docker container automatically.

### Prerequisites

```bash
# Required on host machine:
# 1. Docker (must be running)
# 2. @devcontainers/cli (auto-installed if missing, or: npm install -g @devcontainers/cli)
```

### Quick Start (Host Commands)

```bash
# Build all 4 production packages (~960KB each)
./tools/devcontainer-build.sh package

# Build all 4 dev packages with debug symbols (~1.7MB each)
./tools/devcontainer-build.sh package-dev

# Generate PNG snapshots for all 4 variants
./tools/devcontainer-build.sh test-png

# Generate test videos for all 4 variants
./tools/devcontainer-build.sh test-video

# Full CI pipeline (8 WASM + PNGs + videos)
./tools/devcontainer-build.sh ci
```

### All Available Build Commands (Host)

| Command | Description | Output Location |
|---------|-------------|-----------------|
| `./tools/devcontainer-build.sh build` | Build container image | - |
| `./tools/devcontainer-build.sh wasm` | Build 4 WASM (production) | `build/*.wasm` |
| `./tools/devcontainer-build.sh wasm-debug` | Build 4 WASM (debug) | `build/*_dev.wasm` |
| `./tools/devcontainer-build.sh wasm-all` | Build 8 WASM (both modes) | `build/*.wasm` |
| `./tools/devcontainer-build.sh package` | 4 signed packages (production) | `dist/*.tar` |
| `./tools/devcontainer-build.sh package-dev` | 4 signed packages (debug) | `dist/*-dev.tar` |
| `./tools/devcontainer-build.sh test-png` | Generate PNG snapshots | `dist/*.png` |
| `./tools/devcontainer-build.sh test-video` | Generate test videos | `test/output/*.mp4` |
| `./tools/devcontainer-build.sh proto` | Update proto submodules | `proto/c/`, `proto/ts/` |
| `./tools/devcontainer-build.sh ci` | Full CI pipeline | all outputs |
| `./tools/devcontainer-build.sh all` | Build container + WASM + PNGs | `build/`, `dist/*.png` |
| `./tools/devcontainer-build.sh full` | Build container + WASM + PNGs + videos | all outputs |
| `./tools/devcontainer-build.sh shell` | Interactive container shell | - |
| `./tools/devcontainer-build.sh exec "cmd"` | Run arbitrary command | - |

### Make Targets (inside container only)

These are for CLion/IDE integration. Use `./tools/devcontainer-build.sh shell` first:

```bash
make                    # Build recording_day (default)
make all                # Build all 4 variants
make all BUILD_MODE=dev # Build with debug symbols
make package            # Build + sign recording_day
make package-all        # Build + sign all 4 variants
make package-dev        # Build + sign recording_day (dev)
make package-all-dev    # Build + sign all 4 variants (dev)
make png-all            # Generate PNGs (4 variants)
make video              # Generate videos (4 variants)
make ci                 # Full CI pipeline
```

### Build Modes

| Mode | Flags | WASM Size | Package Size | Features |
|------|-------|-----------|--------------|----------|
| production | `-Oz -flto -DNDEBUG` | ~640 KB | ~900 KB | Optimized, no debug, no LOG_DEBUG/INFO |
| dev | `-O1 -g -D_FORTIFY_SOURCE=2` | ~2.9 MB | ~3 MB | Debug symbols, sanitizers, all logging |

**Logging behavior**:
- Production: `LOG_DEBUG()` and `LOG_INFO()` compile to nothing (like `assert()`)
- Development: All log levels active (`LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`)
- `LOG_WARN()` and `LOG_ERROR()` always active in both modes

**Dead code detection**: Build warns on unused functions/variables in source files (not vendor)

## 4-Variant Architecture

| Variant | Resolution | Timestamp | Navball | Crosshair |
|---------|-----------|-----------|---------|-----------|
| live_day | 1920x1080 | Hidden | Disabled | Vertical (+) |
| live_thermal | 900x720 | Hidden | Disabled | Diagonal (X) |
| recording_day | 1920x1080 | Visible | 5thHorseman_brown | Vertical (+) |
| recording_thermal | 900x720 | Visible | 5thHorseman_v2 | Diagonal (X) |

**Live variant optimization**: Live variants disable navball and celestial indicators for minimal rendering cost (crosshair + variant_info only).

**Compile-time defines**:
- `-DOSD_MODE_LIVE` or `-DOSD_MODE_RECORDING`
- `-DOSD_STREAM_DAY` or `-DOSD_STREAM_THERMAL`

## Widget System

All widgets implement: `bool widget_render(osd_context_t *ctx, osd_state_t *state)`

Returns `true` if something was rendered (enables GPU upload skipping when static).

Note: `variant_info_render()` uses `const osd_state_t *state` since it doesn't modify state.

### Crosshair (`src/widgets/crosshair.c`)
- Primitive rendering (center dot, cross, circle)
- Center offset from protobuf (`rec_osd.day_crosshair_offset_*` or `rec_osd.heat_crosshair_offset_*`)
- Speed indicators with configurable display:
  - Shows only when `rotary.is_moving == true` AND normalized speed > threshold
  - Threshold filtering: `normalized_speed = |speed| / max_speed`, show if > threshold (default 0.05)
  - Radial positioning: 110px horizontal, 90px vertical from center
  - Format: 3 decimal places with 1px black outline
  - Config: `speed_indicators.threshold` (0.0-1.0), `max_speed_azimuth/elevation` (default 35.0)

### Navball (`src/widgets/navball.c`)
- KSP-style orientation indicator
- Equirectangular projection + quaternion rotation (cglm)
- Rotation order: YXZ intrinsic (pitch -> roll -> yaw)
- 13 available skins via registry lookup
- **LUT optimization**: Precomputes ~51,000 sphere geometry entries, eliminating ~70,000 sqrt calls per frame
- Level marker: Optional horizontal white line at center
- Center indicator: Configurable SVG overlay (circle, rectangle, or crosshair)
- Simple diffuse lighting: `0.4 + 0.6 * (N·L)` with light at (0.3, 0.3, 1.0)

### Celestial Indicators (`src/widgets/navball.c`)
- Sun/moon position tracking on navball using astronomy calculations
- Uses `actual_space_time` message for observer location and timestamp
- Front hemisphere (visible): Full size, full opacity
- Back hemisphere (below horizon): 70% size, 50% opacity
- Configurable visibility threshold (e.g., -5.0° shows bodies slightly below horizon)
- 4 SVG variants per body: front, back, circle_front, circle_back

### Timestamp (`src/widgets/timestamp.c`)
- UTC time from protobuf (`time.timestamp`)
- Hidden in live builds, visible in recording builds
- Format: `HH:MM:SS UTC` with 2px black outline
- Configurable position, color, and font size

### Variant Info (`src/widgets/variant_info.c`)
- Debug overlay showing variant name and config values
- Displays: variant, resolution, mode, and widget enable states
- Multi-line text with 1px outline, 4px line spacing

## Protobuf Integration

**Submodules**:
- `proto/c/` - C nanopb bindings (27 file pairs)
- `proto/ts/` - TypeScript bindings (ts-proto)

**Key message**: `JonGUIState` containing:
- `actual_space_time` - **Primary orientation source** for navball: azimuth, elevation, bank, plus observer location (lat/lon/alt) and timestamp for celestial calculations
- `compass` - Alternative orientation (azimuth 0-360, elevation -90 to 90, bank -180 to 180)
- `rotary` - Speed indicators: azimuth_speed, elevation_speed (normalized -1.0 to 1.0), is_moving flag
- `time` - Timestamp display (Unix epoch)
- `rec_osd` - Crosshair offsets: day_crosshair_offset_*, heat_crosshair_offset_*

**Update protos**: `make proto` or `./tools/devcontainer-build.sh proto`

## Configuration (JSON)

Each variant has a config file: `resources/{variant}.json`

**IMPORTANT - Required Fields for Frontend Compatibility**:
The frontend (prototype_basic) validates configs against a JSON schema. These fields are **required**:
- `version` - Semantic version string (e.g., "1.0.0"). **DO NOT REMOVE** - required by frontend schema validation even though the C code doesn't use it.
- Per-widget `font` fields in `timestamp`, `speed_indicators`, and `variant_info` sections.

Key settings:
- `crosshair.orientation` - "vertical" or "diagonal"
- `timestamp.enabled` - true (recording) or false (live)
- `navball.skin` - One of 13 registered skins
- `navball.center_indicator.indicator` - "circle", "rectangle", or "crosshair"
- `speed_indicators.threshold` - Min normalized speed to show (0.0-1.0, default 0.05)
- `speed_indicators.max_speed_azimuth/elevation` - Display scale (default 35.0)
- `celestial_indicators.enabled` - Enable sun/moon tracking
- `celestial_indicators.visibility_threshold` - Altitude threshold for display (e.g., -5.0)
- `celestial_indicators.scale` - Size multiplier for indicators

**Font options** (available for `timestamp.font`, `speed_indicators.font`, `variant_info.font`):
- `liberation_sans_bold` - Default, clean sans-serif
- `b612_mono_bold` - Monospace, designed for cockpit displays
- `share_tech_mono` - Technical monospace
- `orbitron_bold` - Futuristic display font

**Schema**: `resources/schemas/osd_config.schema.json`

**Proto-sourced values** (NOT in config, come from protobuf state at runtime):
- Navball orientation: `actual_space_time.azimuth` / `.elevation` / `.bank`
- Celestial positions: `actual_space_time.latitude` / `.longitude` / `.altitude` / `.timestamp`
- Speed values: `rotary.azimuth_speed` / `rotary.elevation_speed` / `rotary.is_moving`
- Crosshair offset: `rec_osd.day_crosshair_offset_*` / `rec_osd.heat_crosshair_offset_*`
- Timestamp: `time.timestamp`

## Code Organization

```
src/
├── osd_plugin.{c,h}     # Main renderer, WASM exports
├── osd_state.{c,h}      # State accessors (hides protobuf)
├── config_json.{c,h}    # JSON configuration parser
├── core/                # Framebuffer, context
├── rendering/           # Text, blending, primitives
├── widgets/             # Crosshair, navball, timestamp, celestial
├── utils/               # Math, resource lookup, celestial position
└── proto/               # Protobuf (27 files + nanopb)

vendor/
├── cJSON.{c,h}          # JSON parser
├── stb_truetype.h       # Font rendering
├── stb_image.h          # PNG loading
├── nanosvg.h            # SVG rasterization
└── astronomy.{c,h}      # Celestial calculations

test/
├── osd_test.c           # PNG harness (Wasmtime C API, includes benchmarking)
├── video_harness/       # Video generation (GStreamer + Wasmtime)
│   ├── main.c           # Driver with 6 animation types
│   ├── synthetic_state.c # Animated state generator
│   └── gst_pipeline.c   # H.264 encoding pipeline
└── *.json               # Test orientation scenarios
```

## WASM API

```c
// Initialize OSD context (must be called first)
// Returns: 0 on success, non-zero on error
int wasm_osd_init(void);

// Update state from protobuf-encoded JonGUIState
// Returns: 0 on success, non-zero on error
int wasm_osd_update_state(uint32_t state_ptr, uint32_t state_size);

// Render OSD to framebuffer
// Returns: 1 if rendered, 0 if skipped (no changes)
int wasm_osd_render(void);

// Get framebuffer pointer (RGBA, width * height * 4 bytes)
uint32_t wasm_osd_get_framebuffer(void);

// Cleanup and free resources
int wasm_osd_destroy(void);
```

## Packaging

Packages are signed tarballs with JWT manifest:

```
jettison-osd-recording_day-1.0.0.tar
├── manifest.jwt              # RS256-signed metadata
└── recording_day.tar.gz      # WASM + config + resources
```

**Manifest contains**: Version, git SHA, SHA256 checksums, system requirements.

**Keys**: `keys/example-{private,public}.pem` (demo only - generate unique keys for production)

## Dev Container

**Toolchain** (`.devcontainer/Dockerfile`):
- Ubuntu 22.04
- WASI SDK 20
- Wasmtime v27.0.0 + C API
- GStreamer 1.0
- Node.js 20 + TypeScript
- clang-format + clang-tidy

**Entry points**:
- CLion: File -> Remote Development -> Dev Containers
- CLI: `./tools/devcontainer-build.sh shell`

**CLion Git Config Fix**:
CLion 2025.3+ may hang at "Setting global Git config" during dev container setup. This is mitigated by:
1. Host `~/.gitconfig` mounted to container via `devcontainer.json`
2. Fallback git config pre-set in Dockerfile
3. Pre-created `/.jbdevcontainer/config/JetBrains` directory

**Docker BuildKit Note**:
If using a custom buildx builder with `docker-container` driver, switch to default for local builds:
```bash
docker buildx use default
```

## Quality Gates

Every build runs automatically:
1. `clang-format` - GNU Coding Standard
2. `clang-tidy` - Warnings as errors

**Exclusions**: `vendor/`, `proto/`, `svg.c`, `config_json.c`, `resource_lookup.c`

## Output Directories

```
build/       # WASM binaries (~640KB production, ~2.9MB debug)
dist/        # Signed packages (.tar) and PNG snapshots (.png)
test/output/ # Generated test videos (.mp4)
logs/        # Build logs
snapshot/    # PNG harness output (gitignored)
```

## Integration with mp4_sei_extract/prototype_basic

The `prototype_basic` video player project uses OSD packages from this repository for real-time overlay rendering.

### Quick Deploy Workflow

```bash
# 1. Build dev package (in this directory)
./tools/devcontainer-build.sh package-dev

# 2. Copy to prototype_basic
cp dist/jettison-osd-recording_day-1.0.0-dev.tar \
   /path/to/mp4_sei_extract/prototype_basic/web/public/osd/default.tar

# 3. Build and deploy prototype_basic
cd /path/to/mp4_sei_extract/prototype_basic
make deploy       # Dev build
# OR
make deploy-prod  # Production build
```

### Package Location in prototype_basic

```
prototype_basic/
└── web/
    └── public/
        └── osd/
            └── default.tar   # ← Copy package here
```

The player bootstraps from this `default.tar` file on first load, storing it in IndexedDB for subsequent sessions.

### Notes

- **Only `recording_day` variant** is supported for video playback
- Dev packages (~1.7MB) include debug symbols; production (~960KB) is optimized
- After updating the package, users may need to clear browser storage to see changes

---

**Build System**: Make + bash scripts
**Last Updated**: 2025-12-29
