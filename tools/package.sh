#!/bin/bash
# Jettison OSD WASM Packaging Script
# Packages a single variant with all necessary resources and cryptographic signature

set -e  # Exit on error
set -u  # Exit on undefined variable

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
DIST_DIR="$PROJECT_ROOT/dist"
RESOURCES_DIR="$PROJECT_ROOT/resources"
KEYS_DIR="$PROJECT_ROOT/keys"
SCHEMAS_DIR="$RESOURCES_DIR/schemas"

# Manifest format version (not package version)
MANIFEST_VERSION="1.0.0"

# Signing configuration
PRIVATE_KEY="$KEYS_DIR/example-private.pem"
PUBLIC_KEY="$KEYS_DIR/example-public.pem"
SIGN_ALGORITHM="RS256"  # JWT algorithm (RSA-SHA256)

# Build mode (set via second argument or BUILD_MODE env var)
# "production" = optimized build, "dev" = debug build
BUILD_MODE="${BUILD_MODE:-production}"
WASM_SUFFIX=""  # Set based on BUILD_MODE in main()

# ============================================================================
# Variant Resource Mapping
# ============================================================================

# Navball skins per variant
declare -A NAVBALL_SKINS
NAVBALL_SKINS[live_day]="5thHorseman-navball_brownblue_DIF.png"
NAVBALL_SKINS[live_thermal]="5thHorseman_v2-navball.png"
NAVBALL_SKINS[recording_day]="5thHorseman-navball_brownblue_DIF.png"
NAVBALL_SKINS[recording_thermal]="5thHorseman_v2-navball.png"

# Center indicators per variant
declare -A CENTER_INDICATORS
CENTER_INDICATORS[live_day]="circle_indicator.svg"
CENTER_INDICATORS[live_thermal]="rectangle_indicator.svg"
CENTER_INDICATORS[recording_day]="circle_indicator.svg"
CENTER_INDICATORS[recording_thermal]="rectangle_indicator.svg"

# Resolutions per variant
declare -A RESOLUTIONS
RESOLUTIONS[live_day]="1920x1080"
RESOLUTIONS[live_thermal]="900x720"
RESOLUTIONS[recording_day]="1920x1080"
RESOLUTIONS[recording_thermal]="900x720"

# Modes per variant
declare -A MODES
MODES[live_day]="live"
MODES[live_thermal]="live"
MODES[recording_day]="recording"
MODES[recording_thermal]="recording"

# Stream types per variant
declare -A STREAM_TYPES
STREAM_TYPES[live_day]="day"
STREAM_TYPES[live_thermal]="thermal"
STREAM_TYPES[recording_day]="day"
STREAM_TYPES[recording_thermal]="thermal"

# ============================================================================
# Helper Functions
# ============================================================================

log() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

error() {
    echo "[ERROR] $*" >&2
    exit 1
}

compute_sha256() {
    local file="$1"
    sha256sum "$file" | awk '{print $1}'
}

compute_file_size() {
    local file="$1"
    stat -c%s "$file"
}

compute_public_key_fingerprint() {
    openssl rsa -pubin -in "$PUBLIC_KEY" -outform DER 2>/dev/null | sha256sum | awk '{print $1}'
}

# Base64url encoding (RFC 4648)
base64url_encode() {
    openssl base64 -e | tr -d '\n' | tr '+/' '-_' | tr -d '='
}

# Base64url decoding (RFC 4648) - handles missing padding
base64url_decode() {
    local input=$(cat)
    # Convert base64url to base64
    local b64=$(echo -n "$input" | tr '_-' '/+')
    # Add padding if needed
    local pad=$((4 - ${#b64} % 4))
    if [[ $pad -lt 4 ]]; then
        b64="${b64}$(printf '=%.0s' $(seq 1 $pad))"
    fi
    echo -n "$b64" | base64 -d
}

# ============================================================================
# Validation
# ============================================================================

usage() {
    cat <<EOF
Usage: $0 <variant> [build_mode]

Package a WASM variant with all resources and cryptographic signature.

Arguments:
  variant     - Required: One of live_day, live_thermal, recording_day, recording_thermal
  build_mode  - Optional: 'production' (default) or 'dev'

Variants:
  live_day         - Live mode, day stream (1920×1080)
  live_thermal     - Live mode, thermal stream (900×720)
  recording_day    - Recording mode, day stream (1920×1080)
  recording_thermal - Recording mode, thermal stream (900×720)

Build Modes:
  production  - Optimized build (~640KB WASM)
  dev         - Debug build with symbols (~2.9MB WASM)

Examples:
  $0 live_day                 # Package production build
  $0 recording_thermal dev    # Package debug build

Output:
  dist/jettison-osd-{variant}-{version}.tar        (production)
  dist/jettison-osd-{variant}-{version}-dev.tar    (dev)

EOF
    exit 1
}

validate_variant() {
    local variant="$1"
    case "$variant" in
        live_day|live_thermal|recording_day|recording_thermal)
            return 0
            ;;
        *)
            error "Invalid variant: $variant"
            ;;
    esac
}

check_prerequisites() {
    local variant="$1"

    # Check VERSION file
    if [[ ! -f "$PROJECT_ROOT/VERSION" ]]; then
        error "VERSION file not found"
    fi

    # Check WASM binary (with suffix for dev builds)
    local wasm_file="$BUILD_DIR/${variant}${WASM_SUFFIX}.wasm"
    if [[ ! -f "$wasm_file" ]]; then
        error "WASM binary not found: $wasm_file"
        echo "Run 'make all BUILD_MODE=$BUILD_MODE' first" >&2
        exit 1
    fi

    # Check variant config
    if [[ ! -f "$RESOURCES_DIR/${variant}.json" ]]; then
        error "Variant config not found: $RESOURCES_DIR/${variant}.json"
    fi

    # Check keys
    if [[ ! -f "$PRIVATE_KEY" ]] || [[ ! -f "$PUBLIC_KEY" ]]; then
        error "Signing keys not found in $KEYS_DIR"
    fi

    # Check required commands
    for cmd in openssl tar gzip jq; do
        if ! command -v $cmd &> /dev/null; then
            error "Required command not found: $cmd"
        fi
    done
}

# ============================================================================
# Config Schema Validation
# ============================================================================

validate_config_against_schema() {
    local config_file="$1"
    local schema_file="$2"

    log "Validating config against schema..."

    # Try ajv-cli first (npm install -g ajv-cli)
    if command -v ajv &>/dev/null; then
        if ajv validate -s "$schema_file" -d "$config_file" --spec=draft7 2>&1; then
            log "  ✅ Config validated (ajv)"
            return 0
        else
            error "Config validation failed against schema (ajv)"
        fi
    # Fall back to python jsonschema
    elif command -v python3 &>/dev/null; then
        if python3 -c "
import json
import sys
try:
    from jsonschema import validate, ValidationError, Draft7Validator
except ImportError:
    print('Warning: jsonschema not installed, skipping validation', file=sys.stderr)
    sys.exit(0)

with open('$schema_file') as f:
    schema = json.load(f)
with open('$config_file') as f:
    config = json.load(f)

try:
    validate(config, schema, cls=Draft7Validator)
    print('Config validated successfully')
except ValidationError as e:
    print(f'Validation error: {e.message}', file=sys.stderr)
    print(f'Path: {list(e.absolute_path)}', file=sys.stderr)
    sys.exit(1)
" 2>&1; then
            log "  ✅ Config validated (python)"
            return 0
        else
            error "Config validation failed against schema (python)"
        fi
    # No validator available
    else
        log "  ⚠️  No schema validator available (install ajv-cli or python3-jsonschema)"
        log "  Skipping schema validation"
        return 0
    fi
}

# ============================================================================
# Resource Collection
# ============================================================================

collect_resources() {
    local variant="$1"
    local staging_dir="$2"

    log "Collecting resources for variant: $variant"

    # Create directory structure
    mkdir -p "$staging_dir/resources/fonts"
    mkdir -p "$staging_dir/resources/navball_skins"
    mkdir -p "$staging_dir/resources/navball_indicators"
    mkdir -p "$staging_dir/resources/schemas"

    # Copy WASM binary (source uses suffix, destination is always variant.wasm)
    log "  Copying WASM binary..."
    cp "$BUILD_DIR/${variant}${WASM_SUFFIX}.wasm" "$staging_dir/${variant}.wasm"

    # Copy variant config
    log "  Copying variant config..."
    cp "$RESOURCES_DIR/${variant}.json" "$staging_dir/config.json"

    # Copy config schema
    log "  Copying config schema..."
    cp "$SCHEMAS_DIR/osd_config.schema.json" "$staging_dir/config.schema.json"

    # Copy all fonts (schema lists all options)
    log "  Copying all fonts..."
    for font in "$RESOURCES_DIR/fonts"/*.ttf; do
        if [ -f "$font" ]; then
            cp "$font" "$staging_dir/resources/fonts/"
        fi
    done

    # Copy all navball skins (schema lists all options)
    log "  Copying all navball skins..."
    for skin in "$RESOURCES_DIR/navball_skins"/*.png; do
        if [ -f "$skin" ]; then
            cp "$skin" "$staging_dir/resources/navball_skins/"
        fi
    done

    # Copy all navball indicators (center + celestial)
    log "  Copying all navball indicators..."
    for svg in "$RESOURCES_DIR/navball_indicators"/*.svg; do
        if [ -f "$svg" ]; then
            cp "$svg" "$staging_dir/resources/navball_indicators/"
        fi
    done

    # Copy manifest schema
    log "  Copying manifest schema..."
    cp "$SCHEMAS_DIR/manifest.schema.json" \
       "$staging_dir/resources/schemas/manifest.schema.json"

    # Copy pip_override.json (config overrides for PiP views, bundled in all packages)
    log "  Copying pip_override.json..."
    cp "$RESOURCES_DIR/pip_override.json" "$staging_dir/pip_override.json"
}

# ============================================================================
# Archive Creation
# ============================================================================

create_inner_archive() {
    local variant="$1"
    local staging_dir="$2"
    local archive_file="$3"

    log "Creating inner archive: $(basename "$archive_file")"

    # Create tar.gz from staging directory (flatten - no "." entry)
    # List all files/directories at the root level
    (cd "$staging_dir" && tar -czf "$archive_file" *)

    local size=$(compute_file_size "$archive_file")
    local checksum=$(compute_sha256 "$archive_file")

    log "  Archive size: $size bytes"
    log "  SHA256: $checksum"
}

# ============================================================================
# Manifest Generation
# ============================================================================

generate_manifest_json() {
    local variant="$1"
    local staging_dir="$2"
    local inner_archive="$3"
    local output_file="$4"

    log "Generating manifest.json..."

    # Read package version
    local package_version
    package_version=$(cat "$PROJECT_ROOT/VERSION" | tr -d '[:space:]')

    # Get git information
    local git_sha
    local git_branch
    git_sha=$(cd "$PROJECT_ROOT" && git rev-parse --short=7 HEAD 2>/dev/null || echo "unknown")
    git_branch=$(cd "$PROJECT_ROOT" && git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")

    # Use global BUILD_MODE (set from command line or environment)
    local build_mode="$BUILD_MODE"

    # Get current timestamp (ISO 8601 UTC)
    local created_at
    created_at=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

    # Compute archive checksum and size
    local archive_sha256
    local archive_size
    archive_sha256=$(compute_sha256 "$inner_archive")
    archive_size=$(compute_file_size "$inner_archive")

    # Compute WASM binary checksum and size
    local wasm_sha256
    local wasm_size
    wasm_sha256=$(compute_sha256 "$staging_dir/${variant}.wasm")
    wasm_size=$(compute_file_size "$staging_dir/${variant}.wasm")

    # Compute config checksum
    local config_sha256
    config_sha256=$(compute_sha256 "$staging_dir/config.json")

    # Compute public key fingerprint
    local pubkey_fingerprint
    pubkey_fingerprint=$(compute_public_key_fingerprint)

    # Build resources JSON array
    local resources_json="["
    local first=true

    # Enumerate all resource files
    while IFS= read -r -d '' file; do
        local rel_path="${file#$staging_dir/}"
        local sha256=$(compute_sha256 "$file")
        local size=$(compute_file_size "$file")
        local type

        # Determine resource type
        case "$rel_path" in
            resources/fonts/*) type="font" ;;
            resources/navball_skins/*) type="texture" ;;
            resources/navball_indicators/*) type="svg" ;;
            resources/schemas/*|*.schema.json) type="schema" ;;
            *) type="other" ;;
        esac

        # Skip WASM and config (handled separately)
        [[ "$rel_path" == *.wasm ]] && continue
        [[ "$rel_path" == "config.json" ]] && continue

        # Add comma separator (except for first item)
        if [[ "$first" == "true" ]]; then
            first=false
        else
            resources_json+=","
        fi

        # Add resource entry
        resources_json+=$(cat <<EOF

    {
      "path": "$rel_path",
      "sha256": "$sha256",
      "size_bytes": $size,
      "type": "$type"
    }
EOF
        )
    done < <(find "$staging_dir/resources" -type f -print0 2>/dev/null)

    # Also include config.schema.json
    local schema_sha256=$(compute_sha256 "$staging_dir/config.schema.json")
    local schema_size=$(compute_file_size "$staging_dir/config.schema.json")

    if [[ "$first" == "false" ]]; then
        resources_json+=","
    fi

    resources_json+=$(cat <<EOF

    {
      "path": "config.schema.json",
      "sha256": "$schema_sha256",
      "size_bytes": $schema_size,
      "type": "schema"
    }
EOF
    )

    # Also include pip_override.json (config overrides for PiP views)
    local pip_sha256=$(compute_sha256 "$staging_dir/pip_override.json")
    local pip_size=$(compute_file_size "$staging_dir/pip_override.json")

    resources_json+=","
    resources_json+=$(cat <<EOF

    {
      "path": "pip_override.json",
      "sha256": "$pip_sha256",
      "size_bytes": $pip_size,
      "type": "config"
    }
EOF
    )

    resources_json+=$'\n  ]'

    # Generate complete manifest.json
    cat > "$output_file" <<EOF
{
  "manifest_version": "$MANIFEST_VERSION",
  "package": {
    "name": "jettison-osd-${variant}",
    "version": "$package_version",
    "variant": "$variant",
    "created_at": "$created_at",
    "git_sha": "$git_sha",
    "git_branch": "$git_branch",
    "build_mode": "$build_mode"
  },
  "archive": {
    "filename": "${variant}.tar.gz",
    "sha256": "$archive_sha256",
    "size_bytes": $archive_size,
    "compression": "gzip"
  },
  "contents": {
    "wasm": {
      "filename": "${variant}.wasm",
      "sha256": "$wasm_sha256",
      "size_bytes": $wasm_size
    },
    "config": {
      "filename": "config.json",
      "sha256": "$config_sha256",
      "schema_version": "1.0.0"
    },
    "resources": $resources_json
  },
  "system_requirements": {
    "wasm_runtime": "wasmtime >= 27.0.0",
    "resolution": "${RESOLUTIONS[$variant]}",
    "mode": "${MODES[$variant]}",
    "stream_type": "${STREAM_TYPES[$variant]}"
  },
  "signature": {
    "algorithm": "$SIGN_ALGORITHM",
    "public_key_fingerprint": "$pubkey_fingerprint"
  }
}
EOF

    log "  Manifest generated: $output_file"
}

# ============================================================================
# JWT Signing (RS256)
# ============================================================================

create_jwt() {
    local manifest_file="$1"
    local jwt_file="$2"

    log "Creating signed JWT..."

    # JWT header: {"alg":"RS256","typ":"JWT"}
    local header='{"alg":"RS256","typ":"JWT"}'
    local header_b64=$(echo -n "$header" | base64url_encode)

    # JWT payload: manifest.json content
    local payload_b64=$(cat "$manifest_file" | base64url_encode)

    # Create signing input: header.payload
    local signing_input="${header_b64}.${payload_b64}"

    # Sign with RSA-SHA256 (save to temp file to avoid shell null byte issues)
    local temp_sig=$(mktemp)
    echo -n "$signing_input" | openssl dgst -sha256 -sign "$PRIVATE_KEY" -binary > "$temp_sig"
    local signature_b64=$(cat "$temp_sig" | base64url_encode)
    rm -f "$temp_sig"

    # Create JWT: header.payload.signature
    echo -n "${signing_input}.${signature_b64}" > "$jwt_file"

    local jwt_size=$(compute_file_size "$jwt_file")
    log "  JWT created: $jwt_size bytes"
}

verify_jwt() {
    local jwt_file="$1"

    log "Verifying JWT signature..."

    # Split JWT into parts
    local jwt=$(cat "$jwt_file")
    local header_b64=$(echo "$jwt" | cut -d. -f1)
    local payload_b64=$(echo "$jwt" | cut -d. -f2)
    local signature_b64=$(echo "$jwt" | cut -d. -f3)

    # Reconstruct signing input
    local signing_input="${header_b64}.${payload_b64}"

    # Decode signature from base64url to binary file
    local temp_sig=$(mktemp)
    echo -n "$signature_b64" | base64url_decode > "$temp_sig"

    # Verify signature
    if echo -n "$signing_input" | openssl dgst -sha256 -verify "$PUBLIC_KEY" -signature "$temp_sig" &>/dev/null; then
        rm -f "$temp_sig"
        log "  ✅ JWT signature verification passed"
        return 0
    else
        rm -f "$temp_sig"
        error "JWT signature verification failed!"
    fi
}

# ============================================================================
# Outer Archive Creation
# ============================================================================

create_outer_archive() {
    local variant="$1"
    local package_version="$2"
    local temp_dir="$3"
    local output_dir="$4"

    log "Creating outer package archive..."

    # Add "-dev" suffix for dev builds
    local mode_suffix=""
    if [[ "$BUILD_MODE" == "dev" ]]; then
        mode_suffix="-dev"
    fi
    local package_name="jettison-osd-${variant}-${package_version}${mode_suffix}.tar"
    local package_path="$output_dir/$package_name"

    # Create outer tar (uncompressed, contains JWT manifest + inner archive)
    tar -cf "$package_path" \
        -C "$temp_dir" \
        manifest.jwt \
        "${variant}.tar.gz"

    local total_size=$(compute_file_size "$package_path")
    log "  Package created: $package_name"
    log "  Total size: $total_size bytes ($(numfmt --to=iec --suffix=B $total_size))"

    echo "$package_path"
}

# ============================================================================
# Main Packaging Function
# ============================================================================

package_variant() {
    local variant="$1"

    log "=========================================="
    log "  Packaging Variant: $variant"
    log "=========================================="

    # Read package version
    local package_version
    package_version=$(cat "$PROJECT_ROOT/VERSION" | tr -d '[:space:]')

    # Create temporary directory
    local temp_dir
    temp_dir=$(mktemp -d)
    trap "rm -rf '$temp_dir'" EXIT

    local staging_dir="$temp_dir/staging"
    mkdir -p "$staging_dir"

    # Step 1: Collect resources
    collect_resources "$variant" "$staging_dir"

    # Step 1.5: Validate config against schema
    validate_config_against_schema "$staging_dir/config.json" "$staging_dir/config.schema.json"

    # Step 2: Create inner archive
    local inner_archive="$temp_dir/${variant}.tar.gz"
    create_inner_archive "$variant" "$staging_dir" "$inner_archive"

    # Step 3: Generate manifest
    local manifest_file="$temp_dir/manifest.json"
    generate_manifest_json "$variant" "$staging_dir" "$inner_archive" "$manifest_file"

    # Step 4: Create signed JWT
    local jwt_file="$temp_dir/manifest.jwt"
    create_jwt "$manifest_file" "$jwt_file"

    # Step 5: Verify JWT signature
    verify_jwt "$jwt_file"

    # Step 6: Create output directory
    mkdir -p "$DIST_DIR"

    # Step 7: Create outer archive
    local package_path
    package_path=$(create_outer_archive "$variant" "$package_version" "$temp_dir" "$DIST_DIR")

    log "=========================================="
    log "  ✅ Package Complete"
    log "=========================================="
    log "Output: $package_path"
    log ""
}

# ============================================================================
# Main Entry Point
# ============================================================================

main() {
    # Check arguments
    if [[ $# -lt 1 ]] || [[ $# -gt 2 ]]; then
        usage
    fi

    local variant="$1"

    # Handle optional build mode argument
    if [[ $# -ge 2 ]]; then
        BUILD_MODE="$2"
    fi

    # Validate build mode and set WASM suffix
    case "$BUILD_MODE" in
        production)
            WASM_SUFFIX=""
            ;;
        dev)
            WASM_SUFFIX="_dev"
            ;;
        *)
            error "Invalid build mode: $BUILD_MODE (must be 'production' or 'dev')"
            ;;
    esac

    log "Build mode: $BUILD_MODE (WASM suffix: '${WASM_SUFFIX:-none}')"

    # Validate variant
    validate_variant "$variant"

    # Check prerequisites
    check_prerequisites "$variant"

    # Package the variant
    package_variant "$variant"
}

# Run main
main "$@"
