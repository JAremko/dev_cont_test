#!/usr/bin/env bash
# Programmatic dev container build script
# Uses official @devcontainers/cli to build WASM and run tests

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# =============================================================================
# Dependency Check: @devcontainers/cli
# =============================================================================

check_devcontainer_cli() {
    if command -v devcontainer &> /dev/null; then
        echo -e "${GREEN}✓${NC} devcontainer CLI found: $(devcontainer --version)"
        return 0
    else
        echo -e "${YELLOW}⚠${NC} devcontainer CLI not found"
        return 1
    fi
}

check_npm() {
    if command -v npm &> /dev/null; then
        echo -e "${GREEN}✓${NC} npm found: $(npm --version)"
        return 0
    else
        echo -e "${RED}✗${NC} npm not found"
        return 1
    fi
}

check_docker() {
    if command -v docker &> /dev/null; then
        echo -e "${GREEN}✓${NC} Docker found: $(docker --version)"
        if docker ps &> /dev/null; then
            echo -e "${GREEN}✓${NC} Docker daemon is running"
            return 0
        else
            echo -e "${RED}✗${NC} Docker daemon is not running"
            return 1
        fi
    else
        echo -e "${RED}✗${NC} Docker not found"
        return 1
    fi
}

prompt_install_devcontainer_cli() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}devcontainer CLI is required but not installed.${NC}"
    echo ""
    echo "The devcontainer CLI is the official tool for building and running"
    echo "dev containers from the command line. It's maintained by Microsoft"
    echo "and supports the devcontainer.json specification."
    echo ""
    echo "Installation options:"
    echo ""
    echo "  1. Global install (recommended):"
    echo -e "     ${BLUE}npm install -g @devcontainers/cli${NC}"
    echo ""
    echo "  2. Project-local install:"
    echo -e "     ${BLUE}npm install --save-dev @devcontainers/cli${NC}"
    echo ""
    echo "  3. Use npx (no install, runs on-demand):"
    echo -e "     ${BLUE}npx @devcontainers/cli${NC}"
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""

    read -p "Would you like to install it globally now? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${BLUE}Installing @devcontainers/cli globally...${NC}"
        npm install -g @devcontainers/cli
        if check_devcontainer_cli; then
            echo -e "${GREEN}✓${NC} Successfully installed devcontainer CLI"
            return 0
        else
            echo -e "${RED}✗${NC} Installation failed"
            return 1
        fi
    else
        echo -e "${YELLOW}Skipping installation.${NC}"
        echo ""
        echo "You can run this script later after installing, or use:"
        echo -e "  ${BLUE}npx @devcontainers/cli${NC} (runs without installing)"
        return 1
    fi
}

# =============================================================================
# Main Dependency Check
# =============================================================================

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Dev Container Build Script${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "Checking dependencies..."
echo ""

# Check Docker
if ! check_docker; then
    echo ""
    echo -e "${RED}ERROR:${NC} Docker is required but not available."
    echo "Please install Docker and ensure the daemon is running:"
    echo "  https://docs.docker.com/get-docker/"
    exit 1
fi

echo ""

# Check npm
if ! check_npm; then
    echo ""
    echo -e "${RED}ERROR:${NC} npm is required but not installed."
    echo "Please install Node.js (which includes npm):"
    echo "  https://nodejs.org/"
    exit 1
fi

echo ""

# Check devcontainer CLI
if ! check_devcontainer_cli; then
    if ! prompt_install_devcontainer_cli; then
        echo ""
        echo -e "${YELLOW}Cannot proceed without devcontainer CLI.${NC}"
        echo "Exiting."
        exit 1
    fi
fi

echo ""
echo -e "${GREEN}✓ All dependencies satisfied${NC}"
echo ""

# =============================================================================
# Build Functions
# =============================================================================

build_devcontainer() {
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Building dev container image...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""

    cd "$PROJECT_ROOT"
    devcontainer build --workspace-folder .

    echo ""
    echo -e "${GREEN}✓ Dev container built successfully${NC}"
}

run_in_devcontainer() {
    local cmd="$1"
    echo -e "${BLUE}Running in dev container:${NC} $cmd"
    echo ""

    cd "$PROJECT_ROOT"
    # Ensure container is running (up is idempotent - won't rebuild if already running)
    devcontainer up --workspace-folder . > /dev/null 2>&1 || true
    devcontainer exec --workspace-folder . bash -c "$cmd"
}

build_wasm() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Building all 4 WASM variants...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make all"

    echo ""
    echo -e "${GREEN}✓ WASM build complete${NC}"
    echo ""
    echo "Output files:"
    ls -lh "$PROJECT_ROOT/build/"*.wasm 2>/dev/null || echo "  (No WASM files found - build may have failed)"
}

build_wasm_debug() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Building all 4 WASM variants (debug mode)...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make all BUILD_MODE=dev"

    echo ""
    echo -e "${GREEN}✓ WASM debug build complete${NC}"
    echo ""
    echo "Output files:"
    ls -lh "$PROJECT_ROOT/build/"*_dev.wasm 2>/dev/null || echo "  (No WASM files found)"
}

build_wasm_all_modes() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Building all 8 WASM variants (4 variants x 2 modes)...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make all-modes"

    echo ""
    echo -e "${GREEN}✓ All WASM builds complete${NC}"
    echo ""
    echo "Output files:"
    ls -lh "$PROJECT_ROOT/build/"*.wasm 2>/dev/null || echo "  (No WASM files found)"
}

update_proto() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Updating proto submodules...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make proto"

    echo ""
    echo -e "${GREEN}✓ Proto submodules updated${NC}"
}

run_tests_png() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Running PNG tests...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make png-all"

    echo ""
    echo -e "${GREEN}✓ PNG tests complete${NC}"
    echo ""
    echo "Output files:"
    ls -lh "$PROJECT_ROOT/dist/"*.png 2>/dev/null || echo "  (No PNG files found)"
}

run_tests_video() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Running video tests (all 4 variants)...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make video"

    echo ""
    echo -e "${GREEN}✓ Video tests complete${NC}"
    echo ""
    echo "Output files:"
    ls -lh "$PROJECT_ROOT/test/output/"*.mp4 2>/dev/null || echo "  (No video files found)"
}

run_ci() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Running full CI pipeline...${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

    run_in_devcontainer "make ci"

    echo ""
    echo -e "${GREEN}✓ CI pipeline complete${NC}"
}

show_help() {
    cat << EOF
${BLUE}Usage:${NC} $0 [command]

${BLUE}Commands:${NC}
  ${GREEN}build${NC}           Build dev container image
  ${GREEN}wasm${NC}            Build all 4 WASM variants (production mode)
  ${GREEN}wasm-debug${NC}      Build all 4 WASM variants (debug mode)
  ${GREEN}wasm-all${NC}        Build all 8 WASM variants (4 × 2 modes)
  ${GREEN}package${NC}         Build + package all 4 variants (production)
  ${GREEN}package-dev${NC}     Build + package all 4 variants (debug)
  ${GREEN}deploy${NC}          Build + package + deploy (dev builds)
  ${GREEN}deploy-prod${NC}     Build + package + deploy (production builds)
  ${GREEN}proto${NC}           Update proto submodules (proto/c, proto/ts)
  ${GREEN}test-png${NC}        Generate PNG snapshots (all 4 variants)
  ${GREEN}test-video${NC}      Generate test videos (all 4 variants)
  ${GREEN}ci${NC}              Full CI pipeline (proto + 8 WASM + PNGs + videos)
  ${GREEN}all${NC}             Build container + WASM + PNG tests
  ${GREEN}full${NC}            Build container + WASM + all tests (PNG + video)
  ${GREEN}exec <cmd>${NC}      Execute arbitrary command in dev container
  ${GREEN}shell${NC}           Open interactive shell in dev container
  ${GREEN}help${NC}            Show this help message

${BLUE}Examples:${NC}
  $0 build              # Build dev container only
  $0 wasm               # Build 4 WASM variants (production)
  $0 wasm-all           # Build 8 WASM variants (both modes)
  $0 ci                 # Full CI build
  $0 exec "make help"   # Run make help inside container
  $0 shell              # Interactive bash shell

${BLUE}Output Locations:${NC}
  WASM binaries:   build/*.wasm (~640 KB production, ~2.9 MB debug)
  PNG snapshots:   dist/*.png
  Video tests:     test/output/*.mp4

${BLUE}Dependencies:${NC}
  - Docker (required, must be running)
  - npm (required, for devcontainer CLI)
  - @devcontainers/cli (auto-installed if missing)

EOF
}

# =============================================================================
# Main Command Router
# =============================================================================

case "${1:-help}" in
    build)
        build_devcontainer
        ;;

    wasm)
        build_wasm
        ;;

    wasm-debug)
        build_wasm_debug
        ;;

    wasm-all)
        build_wasm_all_modes
        ;;

    package)
        echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${BLUE}Building + packaging all 4 variants (production)...${NC}"
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        run_in_devcontainer "make package-all"
        echo ""
        echo -e "${BLUE}Output packages (production):${NC}"
        ls -lh "$PROJECT_ROOT/dist/"*.tar 2>/dev/null | grep -v dev || echo "  (No production packages found)"
        ;;

    package-dev)
        echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${BLUE}Building + packaging all 4 variants (dev/debug)...${NC}"
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        run_in_devcontainer "make package-all-dev"
        echo ""
        echo -e "${BLUE}Output packages (dev):${NC}"
        ls -lh "$PROJECT_ROOT/dist/"*-dev.tar 2>/dev/null || echo "  (No dev packages found)"
        ;;

    deploy)
        echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${BLUE}Building + deploying (dev builds)...${NC}"
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        run_in_devcontainer "make deploy"
        echo ""
        echo -e "${GREEN}✓ Deploy complete (dev)${NC}"
        ;;

    deploy-prod)
        echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${BLUE}Building + deploying (production builds)...${NC}"
        echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        run_in_devcontainer "make deploy-prod"
        echo ""
        echo -e "${GREEN}✓ Deploy complete (production)${NC}"
        ;;

    proto)
        update_proto
        ;;

    test-png)
        run_tests_png
        ;;

    test-video)
        run_tests_video
        ;;

    ci)
        build_devcontainer
        run_ci
        echo ""
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${GREEN}✓ CI pipeline complete${NC}"
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        ;;

    all)
        build_devcontainer
        build_wasm
        run_tests_png
        echo ""
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${GREEN}✓ Build pipeline complete${NC}"
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        ;;

    full)
        build_devcontainer
        build_wasm
        run_tests_png
        run_tests_video
        echo ""
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${GREEN}✓ Full build + test pipeline complete${NC}"
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        ;;

    exec)
        if [ -z "${2:-}" ]; then
            echo -e "${RED}ERROR:${NC} exec requires a command argument"
            echo "Usage: $0 exec <command>"
            exit 1
        fi
        run_in_devcontainer "$2"
        ;;

    shell)
        echo -e "${BLUE}Opening interactive shell in dev container...${NC}"
        echo -e "${YELLOW}Tip: Type 'exit' to return to host${NC}"
        echo ""
        cd "$PROJECT_ROOT"
        devcontainer exec --workspace-folder . bash
        ;;

    help|--help|-h)
        show_help
        ;;

    *)
        echo -e "${RED}ERROR:${NC} Unknown command: $1"
        echo ""
        show_help
        exit 1
        ;;
esac

echo ""
