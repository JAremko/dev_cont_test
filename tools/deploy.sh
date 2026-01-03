#!/bin/bash
# Jettison OSD WASM Deploy Script
# Deploys signed packages directly to sych.local where nginx serves them

set -e  # Exit on error
set -u  # Exit on undefined variable

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DIST_DIR="$PROJECT_ROOT/dist"

# Load .env if it exists
if [[ -f "$PROJECT_ROOT/.env" ]]; then
    # shellcheck disable=SC1091
    source "$PROJECT_ROOT/.env"
fi

# Remote deployment config
DEPLOY_HOST="${DEPLOY_HOST:-sych.local}"
DEPLOY_USER="${DEPLOY_USER:-archer}"

# Remote paths on sych.local (where nginx serves from)
REMOTE_FRONTEND_PATH="/home/archer/web/www/osd/packages"
REMOTE_GALLERY_PATH="/home/archer/web/www/mp4-sei/osd"

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

usage() {
    cat <<EOF
Usage: $0 <build_mode> [target]

Deploy OSD packages to sych.local where nginx serves them.

Arguments:
  build_mode  - Required: 'dev' or 'production'
  target      - Optional: 'frontend', 'gallery', or omit for both

Targets:
  frontend  - Deploy live_day.tar and live_thermal.tar to www/osd/packages/
  gallery   - Deploy recording_day as default.tar to www/mp4-sei/osd/

Examples:
  $0 dev                     # Deploy dev builds to both targets
  $0 production frontend     # Deploy production builds to frontend only
  $0 dev gallery             # Deploy dev builds to gallery only

EOF
    exit 1
}

# ============================================================================
# Package Naming
# ============================================================================

get_package_name() {
    local variant="$1"
    local build_mode="$2"
    local version
    version=$(cat "$PROJECT_ROOT/VERSION" | tr -d '[:space:]')

    if [[ "$build_mode" == "dev" ]]; then
        echo "jettison-osd-${variant}-${version}-dev.tar"
    else
        echo "jettison-osd-${variant}-${version}.tar"
    fi
}

# ============================================================================
# Deploy Functions
# ============================================================================

deploy_to_frontend() {
    local build_mode="$1"

    log "Deploying to frontend: $DEPLOY_USER@$DEPLOY_HOST:$REMOTE_FRONTEND_PATH"

    # Ensure remote directory exists
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "mkdir -p $REMOTE_FRONTEND_PATH"

    # Deploy live variants
    for variant in live_day live_thermal; do
        local package_name
        package_name=$(get_package_name "$variant" "$build_mode")
        local source_path="$DIST_DIR/$package_name"

        if [[ ! -f "$source_path" ]]; then
            error "Package not found: $source_path"
        fi

        # Rsync to server
        rsync -z "$source_path" "$DEPLOY_USER@$DEPLOY_HOST:$REMOTE_FRONTEND_PATH/${variant}.tar"
        log "  Deployed: $package_name -> ${variant}.tar"
    done

    # Touch all files to ensure new mtime (triggers hot-reload via ETag change)
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "touch $REMOTE_FRONTEND_PATH/live_day.tar $REMOTE_FRONTEND_PATH/live_thermal.tar"
    log "Frontend deploy complete (files touched for hot-reload)"
}

deploy_to_gallery() {
    local build_mode="$1"

    log "Deploying to gallery: $DEPLOY_USER@$DEPLOY_HOST:$REMOTE_GALLERY_PATH"

    # Ensure remote directory exists
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "mkdir -p $REMOTE_GALLERY_PATH"

    # Deploy recording_day as default.tar
    local package_name
    package_name=$(get_package_name "recording_day" "$build_mode")
    local source_path="$DIST_DIR/$package_name"

    if [[ ! -f "$source_path" ]]; then
        error "Package not found: $source_path"
    fi

    # Rsync to server (no -a to avoid preserving timestamps)
    rsync -z "$source_path" "$DEPLOY_USER@$DEPLOY_HOST:$REMOTE_GALLERY_PATH/default.tar"
    log "  Deployed: $package_name -> default.tar"

    log "Gallery deploy complete"
}

# ============================================================================
# Main Entry Point
# ============================================================================

main() {
    # Check arguments
    if [[ $# -lt 1 ]] || [[ $# -gt 2 ]]; then
        usage
    fi

    local build_mode="$1"
    local target="${2:-all}"

    # Validate build mode
    case "$build_mode" in
        dev|production)
            ;;
        *)
            error "Invalid build mode: $build_mode (must be 'dev' or 'production')"
            ;;
    esac

    log "=========================================="
    log "  OSD Package Deploy"
    log "=========================================="
    log "Build mode: $build_mode"
    log "Target: $target"
    log "Source: $DIST_DIR"
    log "Server: $DEPLOY_USER@$DEPLOY_HOST"
    log ""

    # Deploy based on target
    case "$target" in
        frontend)
            deploy_to_frontend "$build_mode"
            ;;
        gallery)
            deploy_to_gallery "$build_mode"
            ;;
        all)
            deploy_to_frontend "$build_mode"
            echo ""
            deploy_to_gallery "$build_mode"
            ;;
        *)
            error "Invalid target: $target (must be 'frontend', 'gallery', or omit for both)"
            ;;
    esac

    log ""
    log "=========================================="
    log "  Deploy Complete"
    log "=========================================="
}

# Run main
main "$@"
