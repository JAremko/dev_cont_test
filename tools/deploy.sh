#!/bin/bash
# Jettison OSD WASM Deploy Script
# Pushes signed packages to Redis on sych.local
# dev_notifications serves packages via HTTP and computes hashes (race-proof)

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

# Redis configuration (from .env - required for hot-reload notifications)
# All packages served from Redis via dev_notifications (no disk storage)
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-8085}"
REDIS_PASSWORD="${REDIS_PASSWORD:-}"

# Validate Redis credentials
if [[ -z "$REDIS_PASSWORD" ]]; then
    echo ""
    echo "=============================================="
    echo "  REDIS CREDENTIALS MISSING"
    echo "=============================================="
    echo ""
    echo "REDIS_PASSWORD is not set in .env"
    echo ""
    echo "Hot-reload notifications require Redis credentials."
    echo "Add the following to your .env file:"
    echo ""
    echo "  REDIS_HOST=127.0.0.1"
    echo "  REDIS_PORT=8085"
    echo "  REDIS_PASSWORD=<password>"
    echo ""
    echo "See CLAUDE.md for Redis port/password reference."
    echo ""
    exit 1
fi

# Use database 1 (Config database) to match dev_notifications service
REDIS_CLI="redis-cli -h $REDIS_HOST -p $REDIS_PORT -a '$REDIS_PASSWORD' -n 1 --no-auth-warning"

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

Deploy OSD packages to Redis on sych.local.
dev_notifications serves packages via /osd/ and handles hot-reload.

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

# Global array to collect deployed package filenames
# Hash is computed by dev_notifications from Redis data (race-proof)
declare -a DEPLOYED_FILES=()

deploy_to_frontend() {
    local build_mode="$1"

    log "Deploying frontend packages to Redis..."

    # Deploy live variants directly to Redis (no disk storage)
    for variant in live_day live_thermal; do
        local package_name
        package_name=$(get_package_name "$variant" "$build_mode")
        local source_path="$DIST_DIR/$package_name"

        if [[ ! -f "$source_path" ]]; then
            error "Package not found: $source_path"
        fi

        # Stream package directly to Redis (dev_notifications computes hash)
        cat "$source_path" | ssh "$DEPLOY_USER@$DEPLOY_HOST" "$REDIS_CLI -x SET osd:package:${variant}.tar" >/dev/null
        log "  Pushed to Redis: osd:package:${variant}.tar ($(stat -c%s "$source_path") bytes)"

        # Track deployed filename for notification
        DEPLOYED_FILES+=("${variant}.tar")
    done

    # Note: pip_override.json is bundled inside packages
    log "Frontend deploy complete"
}

deploy_to_gallery() {
    local build_mode="$1"

    log "Deploying gallery package to Redis..."

    # Deploy recording_day as default.tar directly to Redis
    local package_name
    package_name=$(get_package_name "recording_day" "$build_mode")
    local source_path="$DIST_DIR/$package_name"

    if [[ ! -f "$source_path" ]]; then
        error "Package not found: $source_path"
    fi

    # Stream package directly to Redis (dev_notifications computes hash)
    cat "$source_path" | ssh "$DEPLOY_USER@$DEPLOY_HOST" "$REDIS_CLI -x SET osd:package:default.tar" >/dev/null
    log "  Pushed to Redis: osd:package:default.tar ($(stat -c%s "$source_path") bytes)"

    # Track deployed filename for notification
    DEPLOYED_FILES+=("default.tar")

    log "Gallery deploy complete"
}

# Notify dev_notifications service to reload packages from Redis
# dev_notifications loads packages and computes hashes (race-proof)
# Format: "file1,file2,..." (just filenames, no hashes)
notify_reload() {
    log "Notifying dev_notifications service to reload packages..."

    # Build payload from deployed filenames (comma-separated)
    local payload=""
    for file in "${DEPLOYED_FILES[@]}"; do
        if [[ -n "$payload" ]]; then
            payload+=","
        fi
        payload+="$file"
    done

    if [[ -z "$payload" ]]; then
        payload="all"
    fi

    ssh "$DEPLOY_USER@$DEPLOY_HOST" "$REDIS_CLI PUBLISH osd:reload '$payload'" >/dev/null
    log "✓ Reload notification sent: $payload"
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
