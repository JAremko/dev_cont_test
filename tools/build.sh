#!/bin/bash
# Compile a single WASM variant
# Supports 4 variants: live/recording × day/thermal
#
# Note: Format and lint are handled by the Makefile (runs once before parallel builds)
# This script only compiles - call `make quality` separately if needed.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Parse variant parameter (default: recording_day for backwards compatibility)
VARIANT="${VARIANT:-recording_day}"

echo "Building: $VARIANT"
echo "--------------------------------------"

# Variant-specific build directory (avoids collisions in parallel builds)
BUILD_SUBDIR="build/${VARIANT}"
mkdir -p "$BUILD_SUBDIR"/{core,rendering,widgets,resources,utils,proto,vendor}

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

# Copy variant config to variant-specific build dir
echo "Using config: $CONFIG_SOURCE"
cp "$PROJECT_ROOT/$CONFIG_SOURCE" "$PROJECT_ROOT/$BUILD_SUBDIR/resources/config.json"

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
# In devcontainer, submodule's .git points to parent which isn't mounted,
# but we mount the git dir to /workspaces/.git_modules
if [ -d "/workspaces/.git_modules" ]; then
    # Read HEAD directly - worktree relative path doesn't work in container
    HEAD_CONTENT=$(cat /workspaces/.git_modules/HEAD 2>/dev/null)
    if [[ "$HEAD_CONTENT" == ref:* ]]; then
        # HEAD points to a ref, resolve it
        REF_PATH="${HEAD_CONTENT#ref: }"
        GIT_COMMIT=$(cat "/workspaces/.git_modules/$REF_PATH" 2>/dev/null | cut -c1-8)
    else
        # Detached HEAD - content is the commit hash
        GIT_COMMIT=$(echo "$HEAD_CONTENT" | cut -c1-8)
    fi
    [ -z "$GIT_COMMIT" ] && GIT_COMMIT="unknown"
else
    GIT_COMMIT=$(git rev-parse --short=8 HEAD 2>/dev/null || echo "unknown")
fi
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
  # Preserve directory structure: src/core/foo.c → build/{variant}/core/foo.o
  rel_path="${c_file#src/}"  # Remove src/ prefix
  obj_file="$BUILD_SUBDIR/${rel_path%.c}.o"

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
for c_file in $PROTO_FILES; do
  obj_file="$BUILD_SUBDIR/proto/$(basename ${c_file%.c}.o)"
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
for c_file in $VENDOR_FILES; do
  obj_file="$BUILD_SUBDIR/vendor/$(basename ${c_file%.c}.o)"
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
