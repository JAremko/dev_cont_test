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

# Centralized OSD path on sych.local (nginx proxies to dev_notifications, files also on disk)
REMOTE_OSD_PATH="/home/archer/web/osd"

# Redis configuration (same as dev_notifications service)
REDIS_HOST="127.0.0.1"
REDIS_PORT="8085"
REDIS_PASSWORD="VFDGOZlbwfSXk5p"
REDIS_CLI="redis-cli -h $REDIS_HOST -p $REDIS_PORT -a '$REDIS_PASSWORD' --no-auth-warning"

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

Deploy OSD packages to sych.local centralized OSD directory.
All packages served from /osd/ by nginx.

Arguments:
  build_mode  - Required: 'dev' or 'production'
  target      - Optional: 'frontend', 'gallery', or omit for all

Targets:
  frontend  - Deploy live_day.tar and live_thermal.tar
  gallery   - Deploy recording_day as default.tar
  all       - Deploy all variants (default)

Examples:
  $0 dev                     # Deploy dev builds (all variants)
  $0 production frontend     # Deploy production builds (frontend only)
  $0 dev gallery             # Deploy dev builds (gallery only)

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
# SSH Connectivity Check
# ============================================================================

check_ssh() {
    log "Checking SSH connectivity to $DEPLOY_USER@$DEPLOY_HOST..."

    if ! ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
         "$DEPLOY_USER@$DEPLOY_HOST" "echo 'SSH OK'" >/dev/null 2>&1; then
        echo ""
        echo "=============================================="
        echo "  SSH CONNECTION FAILED"
        echo "=============================================="
        echo ""
        echo "Could not connect to: $DEPLOY_USER@$DEPLOY_HOST"
        echo ""
        echo "Possible causes:"
        echo "  1. SSH key requires passphrase but ssh-agent not running"
        echo "  2. SSH key not added to agent"
        echo "  3. Host unreachable (check /etc/hosts or network)"
        echo "  4. SSH key not authorized on remote host"
        echo ""
        echo "To fix passphrase-protected SSH keys:"
        echo "  eval \$(ssh-agent)     # Start ssh-agent (if not running)"
        echo "  ssh-add ~/.ssh/id_*   # Add your key (will prompt for passphrase once)"
        echo ""
        echo "To test manually:"
        echo "  ssh $DEPLOY_USER@$DEPLOY_HOST"
        echo ""
        exit 1
    fi

    log "✓ SSH connection verified"
}

# ============================================================================
# Deploy Functions
# ============================================================================

deploy_to_frontend() {
    local build_mode="$1"

    log "Deploying frontend packages to: $DEPLOY_USER@$DEPLOY_HOST:$REMOTE_OSD_PATH"

    # Ensure remote directory exists
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "mkdir -p $REMOTE_OSD_PATH"

    # Deploy live variants
    for variant in live_day live_thermal; do
        local package_name
        package_name=$(get_package_name "$variant" "$build_mode")
        local source_path="$DIST_DIR/$package_name"

        if [[ ! -f "$source_path" ]]; then
            error "Package not found: $source_path"
        fi

        # Rsync to disk (for backup/debugging)
        rsync -z --chmod=F644 "$source_path" "$DEPLOY_USER@$DEPLOY_HOST:$REMOTE_OSD_PATH/${variant}.tar"
        log "  Deployed to disk: $package_name -> ${variant}.tar"

        # Push to Redis for atomic serving
        ssh "$DEPLOY_USER@$DEPLOY_HOST" "$REDIS_CLI -x SET osd:package:${variant}.tar < $REMOTE_OSD_PATH/${variant}.tar" >/dev/null
        log "  Pushed to Redis: osd:package:${variant}.tar"
    done

    # Note: pip_override.json is now bundled inside packages
    log "Frontend deploy complete"
}

deploy_to_gallery() {
    local build_mode="$1"

    log "Deploying gallery package to: $DEPLOY_USER@$DEPLOY_HOST:$REMOTE_OSD_PATH"

    # Ensure remote directory exists
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "mkdir -p $REMOTE_OSD_PATH"

    # Deploy recording_day as default.tar
    local package_name
    package_name=$(get_package_name "recording_day" "$build_mode")
    local source_path="$DIST_DIR/$package_name"

    if [[ ! -f "$source_path" ]]; then
        error "Package not found: $source_path"
    fi

    # Rsync to disk (for backup/debugging)
    rsync -z --chmod=F644 "$source_path" "$DEPLOY_USER@$DEPLOY_HOST:$REMOTE_OSD_PATH/default.tar"
    log "  Deployed to disk: $package_name -> default.tar"

    # Push to Redis for atomic serving
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "$REDIS_CLI -x SET osd:package:default.tar < $REMOTE_OSD_PATH/default.tar" >/dev/null
    log "  Pushed to Redis: osd:package:default.tar"

    log "Gallery deploy complete"
}

# Notify dev_notifications service to reload packages from Redis
notify_reload() {
    log "Notifying dev_notifications service to reload packages..."
    ssh "$DEPLOY_USER@$DEPLOY_HOST" "$REDIS_CLI PUBLISH osd:reload all" >/dev/null
    log "✓ Reload notification sent"
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

    # Pre-flight SSH check
    check_ssh

    # Deploy based on target
    case "$target" in
        frontend)
            deploy_to_frontend "$build_mode"
            notify_reload
            ;;
        gallery)
            deploy_to_gallery "$build_mode"
            notify_reload
            ;;
        all)
            deploy_to_frontend "$build_mode"
            echo ""
            deploy_to_gallery "$build_mode"
            notify_reload
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
