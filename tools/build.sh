#!/bin/bash
# Build workflow: format → lint → compile
# Supports 4 variants: live/recording × day/thermal

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Parse variant parameter (default: recording_day for backwards compatibility)
VARIANT="${VARIANT:-recording_day}"

echo "=========================================="
echo "  WASM OSD Build Workflow"
echo "  Variant: $VARIANT"
echo "=========================================="
echo ""

# Step 1: Format
echo "Step 1/3: Formatting source files..."
echo "--------------------------------------"
if ! ./tools/format.sh; then
  echo "❌ Formatting failed"
  exit 1
fi
echo ""

# Step 2: Lint
echo "Step 2/3: Linting source files..."
echo "--------------------------------------"
if ! ./tools/lint.sh; then
  echo "❌ Linting failed"
  exit 1
fi
echo ""

# Step 3: Compile
echo "Step 3/3: Compiling to WASM..."
echo "--------------------------------------"

# Check for build directory and subdirectories
if [ ! -d "build" ]; then
  mkdir -p build
  echo "Created build directory"
fi

# Create build subdirectories for modular organization
mkdir -p build/core
mkdir -p build/rendering
mkdir -p build/widgets
mkdir -p build/resources
mkdir -p build/utils
mkdir -p build/proto
mkdir -p build/vendor

# Parse variant to determine build flags
case "$VARIANT" in
  live_day)
    WIDTH=1920
    HEIGHT=1080
    VARIANT_DEFINES="-DOSD_MODE_LIVE -DOSD_STREAM_DAY -DCURRENT_FRAMEBUFFER_WIDTH=$WIDTH -DCURRENT_FRAMEBUFFER_HEIGHT=$HEIGHT"
    VARIANT_DESC="Live + Day (1920×1080)"
    CONFIG_SOURCE="resources/live_day.json"
    ;;
  live_thermal)
    WIDTH=900
    HEIGHT=720
    VARIANT_DEFINES="-DOSD_MODE_LIVE -DOSD_STREAM_THERMAL -DCURRENT_FRAMEBUFFER_WIDTH=$WIDTH -DCURRENT_FRAMEBUFFER_HEIGHT=$HEIGHT"
    VARIANT_DESC="Live + Thermal (900×720)"
    CONFIG_SOURCE="resources/live_thermal.json"
    ;;
  recording_day)
    WIDTH=1920
    HEIGHT=1080
    VARIANT_DEFINES="-DOSD_MODE_RECORDING -DOSD_STREAM_DAY -DCURRENT_FRAMEBUFFER_WIDTH=$WIDTH -DCURRENT_FRAMEBUFFER_HEIGHT=$HEIGHT"
    VARIANT_DESC="Recording + Day (1920×1080)"
    CONFIG_SOURCE="resources/recording_day.json"
    ;;
  recording_thermal)
    WIDTH=900
    HEIGHT=720
    VARIANT_DEFINES="-DOSD_MODE_RECORDING -DOSD_STREAM_THERMAL -DCURRENT_FRAMEBUFFER_WIDTH=$WIDTH -DCURRENT_FRAMEBUFFER_HEIGHT=$HEIGHT"
    VARIANT_DESC="Recording + Thermal (900×720)"
    CONFIG_SOURCE="resources/recording_thermal.json"
    ;;
  *)
    echo "❌ Unknown variant: $VARIANT"
    echo "Valid variants: live_day, live_thermal, recording_day, recording_thermal"
    exit 1
    ;;
esac

# Copy variant-specific config to build/resources/
# Both generic config.json and variant-specific {variant}_config.json
echo "Using config: $CONFIG_SOURCE"
mkdir -p "$PROJECT_ROOT/build/resources"
cp "$PROJECT_ROOT/$CONFIG_SOURCE" "$PROJECT_ROOT/build/resources/config.json"
cp "$PROJECT_ROOT/$CONFIG_SOURCE" "$PROJECT_ROOT/build/resources/${VARIANT}_config.json"
echo "Saved variant config: build/resources/${VARIANT}_config.json"

# Determine build mode (default: production)
BUILD_MODE="${BUILD_MODE:-production}"

# Hardening flags (empty for production, enabled for dev)
HARDENING_FLAGS=""

# Dead code detection flags (both modes - helps keep codebase clean)
# These warnings catch unused functions, variables, and unreachable code
DEAD_CODE_FLAGS="-Wunused-function -Wunused-variable -Wunused-but-set-variable"

case "$BUILD_MODE" in
  dev)
    echo "Building in DEVELOPMENT mode (debugging + hardening)"
    OPTIMIZATION="-O1 -g"
    OUTPUT_SUFFIX="_dev"

    # WASM-compatible hardening flags (ALWAYS enabled in dev)
    # Note: UBSan not available in WASI SDK 20 (missing runtime library)
    HARDENING_FLAGS="-D_FORTIFY_SOURCE=2 -fstack-protector-strong"

    echo "  + Optimization: -O1 (debug-friendly)"
    echo "  + Debug symbols: -g"
    echo "  + Buffer overflow detection: _FORTIFY_SOURCE=2"
    echo "  + Stack canaries: stack-protector-strong"
    echo ""
    echo "  ⚠️  This build will TRAP on buffer overflows!"
    echo "  Note: UBSan not available (WASI SDK limitation)"
    echo ""

    ;;

  production)
    echo "Building in PRODUCTION mode (optimized, no hardening)"
    OPTIMIZATION="-Oz -flto -DNDEBUG"
    OUTPUT_SUFFIX=""
    echo "  + Maximum optimization: -Oz + LTO"
    echo "  + No debug symbols"
    echo "  + No sanitizers (performance-critical)"
    echo ""
    ;;

  *)
    echo "❌ Unknown build mode: $BUILD_MODE"
    echo "Valid modes: dev, production"
    exit 1
    ;;
esac

echo "Variant: $VARIANT_DESC"
echo "Resolution: ${WIDTH}×${HEIGHT}"
echo ""

# Check for WASI SDK
if [ -z "$WASI_SDK_PATH" ]; then
  echo "❌ WASI_SDK_PATH not set"
  echo "Please set WASI_SDK_PATH or run inside Docker container"
  exit 1
fi

if [ ! -f "$WASI_SDK_PATH/bin/clang" ]; then
  echo "❌ WASI SDK not found at $WASI_SDK_PATH"
  exit 1
fi

echo "Using WASI SDK: $WASI_SDK_PATH"
echo ""

# Build metadata (for variant_info display)
GIT_COMMIT=$(git rev-parse --short=8 HEAD 2>/dev/null || echo "unknown")
BUILD_DATE=$(date -u '+%Y-%m-%d')
BUILD_TIME=$(date -u '+%H:%M:%S')
VERSION="1.0.0"  # Could read from VERSION file if needed

echo "Build Metadata:"
echo "  Version: $VERSION"
echo "  Commit: $GIT_COMMIT"
echo "  Date: $BUILD_DATE $BUILD_TIME UTC"
echo ""

# Version defines for variant_info
VERSION_DEFINES="-DOSD_VERSION=\"$VERSION\" -DOSD_GIT_COMMIT=\"$GIT_COMMIT\" -DOSD_BUILD_DATE=\"$BUILD_DATE\" -DOSD_BUILD_TIME=\"$BUILD_TIME\""

# Find all .c files in src/ (excluding proto/ and utils.c which is not used)
C_FILES=$(find src -name "*.c" -not -path "*/proto/*" -not -name "utils.c" 2>/dev/null || true)
# Include all proto files (both *.pb.c and pb*.c for nanopb library)
PROTO_FILES=$(find src/proto -name "*.c" 2>/dev/null || true)
# Find vendor files (excluding test directories)
VENDOR_FILES=$(find vendor -name "*.c" -not -path "*/test/*" -not -path "*/docs/*" 2>/dev/null || true)

# Compile main source files
OBJECT_FILES=""
for c_file in $C_FILES; do
  # Preserve directory structure: src/core/foo.c → build/core/foo.o
  rel_path="${c_file#src/}"  # Remove src/ prefix
  obj_file="build/${rel_path%.c}.o"

  echo "  Compiling: $c_file → $obj_file"

  "$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    --sysroot="$WASI_SDK_PATH/share/wasi-sysroot" \
    -I"$PROJECT_ROOT/src" \
    -I"$PROJECT_ROOT/src/core" \
    -I"$PROJECT_ROOT/src/rendering" \
    -I"$PROJECT_ROOT/src/widgets" \
    -I"$PROJECT_ROOT/src/resources" \
    -I"$PROJECT_ROOT/src/utils" \
    -I"$PROJECT_ROOT/src/proto" \
    -I"$PROJECT_ROOT/vendor" \
    -I"$PROJECT_ROOT/vendor/cglm/include" \
    $OPTIMIZATION \
    $HARDENING_FLAGS \
    $DEAD_CODE_FLAGS \
    -DWASI_BUILD \
    $VARIANT_DEFINES \
    $VERSION_DEFINES \
    -c "$c_file" \
    -o "$obj_file"

  OBJECT_FILES="$OBJECT_FILES $obj_file"
done

# Compile proto files
mkdir -p build/proto
for c_file in $PROTO_FILES; do
  obj_file="build/proto/$(basename ${c_file%.c}.o)"
  echo "  Compiling: $c_file → $obj_file"

  "$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    --sysroot="$WASI_SDK_PATH/share/wasi-sysroot" \
    -I"$PROJECT_ROOT/src" \
    -I"$PROJECT_ROOT/src/proto" \
    -I"$PROJECT_ROOT/vendor" \
    -I"$PROJECT_ROOT/vendor/cglm/include" \
    $OPTIMIZATION \
    $HARDENING_FLAGS \
    $DEAD_CODE_FLAGS \
    -DWASI_BUILD \
    $VARIANT_DEFINES \
    $VERSION_DEFINES \
    -c "$c_file" \
    -o "$obj_file"

  OBJECT_FILES="$OBJECT_FILES $obj_file"
done

# Compile vendor files (no DEAD_CODE_FLAGS - third-party code we don't control)
mkdir -p build/vendor
for c_file in $VENDOR_FILES; do
  obj_file="build/vendor/$(basename ${c_file%.c}.o)"
  echo "  Compiling: $c_file → $obj_file"

  "$WASI_SDK_PATH/bin/clang" \
    --target=wasm32-wasi \
    --sysroot="$WASI_SDK_PATH/share/wasi-sysroot" \
    -I"$PROJECT_ROOT/src" \
    -I"$PROJECT_ROOT/src/proto" \
    -I"$PROJECT_ROOT/vendor" \
    -I"$PROJECT_ROOT/vendor/cglm/include" \
    $OPTIMIZATION \
    $HARDENING_FLAGS \
    -DWASI_BUILD \
    -DASTRONOMY_ENGINE_WHOLE_SECOND \
    $VARIANT_DEFINES \
    -c "$c_file" \
    -o "$obj_file"

  OBJECT_FILES="$OBJECT_FILES $obj_file"
done

echo ""
echo "Variant Configuration:"
echo "  Defines: $VARIANT_DEFINES"
echo "  Resolution: ${WIDTH}×${HEIGHT}"
echo "  Output: build/${VARIANT}${OUTPUT_SUFFIX}.wasm"

# Link to WASM
OUTPUT_WASM="build/${VARIANT}${OUTPUT_SUFFIX}.wasm"
echo ""
echo "  Linking: $OUTPUT_WASM"

"$WASI_SDK_PATH/bin/clang" \
  --target=wasm32-wasi \
  --sysroot="$WASI_SDK_PATH/share/wasi-sysroot" \
  $OPTIMIZATION \
  $HARDENING_FLAGS \
  -nostartfiles \
  -Wl,--export-all \
  -Wl,--no-entry \
  $OBJECT_FILES \
  -lm \
  -o "$OUTPUT_WASM"

# Get file size
if stat --version &>/dev/null 2>&1; then
  # GNU stat (Linux)
  WASM_SIZE=$(stat -c%s "$OUTPUT_WASM")
else
  # BSD stat (macOS)
  WASM_SIZE=$(stat -f%z "$OUTPUT_WASM")
fi

# Convert to KB
WASM_SIZE_KB=$((WASM_SIZE / 1024))

echo ""
echo "=========================================="
echo "✅ Build complete: $VARIANT"
echo "=========================================="
echo ""
echo "Output: $OUTPUT_WASM"
echo "Size: $WASM_SIZE_KB KB"
echo "Resolution: ${WIDTH}×${HEIGHT}"
echo ""
