# Jettison WASM OSD - Makefile
# Build system for CLion/terminal development
#
# Main workflow:
#   make                    - Build recording_day (quality + compile)
#   make all                - Build all 4 variants (parallel, quality runs once)
#   make package-all        - Build + package all variants
#   make deploy             - Build + package + deploy all
#   make clean              - Clean all artifacts
#
# Quality (format + lint) runs ONCE before multi-variant builds.
# Parallel builds use JOBS=8 by default (override with: make all JOBS=N)
#
# Environment Detection:
#   - Host: Delegates to ./tools/devcontainer-build.sh (Docker-based)
#   - Devcontainer: Runs commands directly (CLion/IDE workflow)

.PHONY: all default build clean clean-wasm clean-packages clean-artifacts help \
        index \
        format lint quality \
        recording_day recording_thermal live_day live_thermal \
        _recording_day _recording_thermal _live_day _live_thermal \
        recording_day_dev recording_thermal_dev live_day_dev live_thermal_dev all-dev \
        _recording_day_dev _recording_thermal_dev _live_day_dev _live_thermal_dev \
        package package-all package-dev package-all-dev \
        deploy deploy-prod deploy-frontend deploy-frontend-prod deploy-gallery deploy-gallery-prod \
        harness video-harness png-harness png png-all video video-all \
        proto ci all-modes png-all-modes

#==============================================================================
# Environment Detection
#==============================================================================

# Detect if running inside devcontainer:
# 1. /workspaces directory exists (standard devcontainer mount point)
# 2. REMOTE_CONTAINERS env var is set (VS Code Remote Containers)
DEVCONTAINER := $(shell if [ -d "/workspaces" ] || [ -n "$$REMOTE_CONTAINERS" ]; then echo "1"; fi)

ifeq ($(DEVCONTAINER),1)
  BUILD_ENV := devcontainer
else
  BUILD_ENV := host
endif

# Parallel jobs for multi-variant builds (override with: make all JOBS=N)
JOBS ?= 8

#==============================================================================
# Configuration
#==============================================================================

# Default variant
VARIANT ?= recording_day

# Build mode: production (default) or dev
BUILD_MODE ?= production

# Directories
PROJECT_ROOT := $(CURDIR)
BUILD_DIR := $(PROJECT_ROOT)/build
DIST_DIR := $(PROJECT_ROOT)/dist
LOGS_DIR := $(PROJECT_ROOT)/logs
SNAPSHOT_DIR := $(PROJECT_ROOT)/snapshot

# WASI SDK (inside dev container)
WASI_SDK_PATH ?= /opt/wasi-sdk

# Native compiler for test harnesses
NATIVE_CC := gcc

# Create logs directory (only in devcontainer)
ifeq ($(DEVCONTAINER),1)
$(shell mkdir -p $(LOGS_DIR))
endif

#==============================================================================
# Default Target
#==============================================================================

default: recording_day

#==============================================================================
# IDE Support (compile_commands.json for CLion/clangd)
#==============================================================================

index:
	@echo "=== Generating compile_commands.json (all 4 variants) ==="
	@mkdir -p $(LOGS_DIR)
	@echo "[]" > compile_commands.json
	@cp compile_commands.json prev_compile_commands.json
	@for variant in live_day live_thermal recording_day recording_thermal; do \
		echo "Indexing: $$variant..."; \
		rm -rf $(BUILD_DIR)/$$variant; \
		bear -- $(MAKE) --no-print-directory _$${variant} BUILD_MODE=dev 2>&1 | tee $(LOGS_DIR)/index_$$variant.log; \
		if [ -f compile_commands.json ]; then \
			jq -s '.[0] + .[1]' prev_compile_commands.json compile_commands.json > temp_compile_commands.json && \
			mv temp_compile_commands.json compile_commands.json; \
		fi; \
		cp compile_commands.json prev_compile_commands.json; \
	done
	@rm -f prev_compile_commands.json
	@echo ""
	@echo "Generated: compile_commands.json (merged from 4 variants)"
	@echo "CLion: File → Reload Compilation Database (or it auto-detects)"

#==============================================================================
# Quality Targets (run automatically before builds)
#==============================================================================

format:
	@echo "=== Formatting ==="
	@./tools/format.sh 2>&1 | tee $(LOGS_DIR)/format.log
	@echo ""

lint:
	@echo "=== Linting ==="
	@./tools/lint.sh 2>&1 | tee $(LOGS_DIR)/lint.log
	@echo ""

quality: format lint

#==============================================================================
# Build Targets
#==============================================================================

# Public targets: run quality first, then compile (for single-variant builds)
recording_day: quality _recording_day
recording_thermal: quality _recording_thermal
live_day: quality _live_day
live_thermal: quality _live_thermal

# Internal targets: compile only (called by parallel builds)
_recording_day:
	@VARIANT=recording_day BUILD_MODE=$(BUILD_MODE) ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_recording_day.log

_recording_thermal:
	@VARIANT=recording_thermal BUILD_MODE=$(BUILD_MODE) ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_recording_thermal.log

_live_day:
	@VARIANT=live_day BUILD_MODE=$(BUILD_MODE) ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_live_day.log

_live_thermal:
	@VARIANT=live_thermal BUILD_MODE=$(BUILD_MODE) ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_live_thermal.log

ifeq ($(DEVCONTAINER),1)
# --- Devcontainer: Build all 4 variants in parallel ---
# Clean WASM files first to ensure no stale binaries can be packaged
all: quality
	@echo "=== Building all 4 variants ($(JOBS) parallel jobs) ==="
	@rm -f $(BUILD_DIR)/*.wasm
	@$(MAKE) -j$(JOBS) --no-print-directory _live_day _live_thermal _recording_day _recording_thermal BUILD_MODE=$(BUILD_MODE)
	@echo ""
	@echo "=== All 4 variants built ==="
	@ls -lh $(BUILD_DIR)/*.wasm 2>/dev/null || true
else
# --- Host: Delegate to devcontainer script ---
all:
	@./tools/devcontainer-build.sh wasm
endif

# Dev build targets (debug builds with symbols, ~2.9MB each)
recording_day_dev: quality _recording_day_dev
recording_thermal_dev: quality _recording_thermal_dev
live_day_dev: quality _live_day_dev
live_thermal_dev: quality _live_thermal_dev

_recording_day_dev:
	@VARIANT=recording_day BUILD_MODE=dev ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_recording_day_dev.log

_recording_thermal_dev:
	@VARIANT=recording_thermal BUILD_MODE=dev ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_recording_thermal_dev.log

_live_day_dev:
	@VARIANT=live_day BUILD_MODE=dev ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_live_day_dev.log

_live_thermal_dev:
	@VARIANT=live_thermal BUILD_MODE=dev ./tools/build.sh 2>&1 | tee $(LOGS_DIR)/build_live_thermal_dev.log

ifeq ($(DEVCONTAINER),1)
# --- Devcontainer: Build all 4 dev variants in parallel ---
# Clean WASM files first to ensure no stale binaries can be packaged
all-dev: quality
	@echo "=== Building all 4 variants (dev, $(JOBS) parallel jobs) ==="
	@rm -f $(BUILD_DIR)/*_dev.wasm
	@$(MAKE) -j$(JOBS) --no-print-directory _live_day_dev _live_thermal_dev _recording_day_dev _recording_thermal_dev
	@echo ""
	@echo "=== All 4 variants built (dev) ==="
	@ls -lh $(BUILD_DIR)/*_dev.wasm 2>/dev/null || true
else
# --- Host: Delegate to devcontainer script ---
all-dev:
	@./tools/devcontainer-build.sh wasm-debug
endif

#==============================================================================
# Package Targets
#==============================================================================

package: recording_day
	@echo "=== Packaging recording_day ==="
	@mkdir -p $(DIST_DIR)
	@./tools/package.sh recording_day 2>&1 | tee $(LOGS_DIR)/package_recording_day.log
	@echo ""
	@ls -lh $(DIST_DIR)/*.tar 2>/dev/null || true

ifeq ($(DEVCONTAINER),1)
package-all: all
	@echo "=== Packaging all 4 variants ==="
	@rm -f $(DIST_DIR)/jettison-osd-*.tar
	@mkdir -p $(DIST_DIR)
	@./tools/package.sh live_day 2>&1 | tee $(LOGS_DIR)/package_live_day.log
	@./tools/package.sh live_thermal 2>&1 | tee $(LOGS_DIR)/package_live_thermal.log
	@./tools/package.sh recording_day 2>&1 | tee $(LOGS_DIR)/package_recording_day.log
	@./tools/package.sh recording_thermal 2>&1 | tee $(LOGS_DIR)/package_recording_thermal.log
	@echo ""
	@ls -lh $(DIST_DIR)/*.tar 2>/dev/null || true
else
package-all:
	@./tools/devcontainer-build.sh package
endif

# Dev package targets (debug builds with symbols)
package-dev: recording_day_dev
	@echo "=== Packaging recording_day (dev) ==="
	@mkdir -p $(DIST_DIR)
	@./tools/package.sh recording_day dev 2>&1 | tee $(LOGS_DIR)/package_recording_day_dev.log
	@echo ""
	@ls -lh $(DIST_DIR)/*-dev.tar 2>/dev/null || true

ifeq ($(DEVCONTAINER),1)
package-all-dev: all-dev
	@echo "=== Packaging all 4 variants (dev) ==="
	@rm -f $(DIST_DIR)/*-dev.tar
	@mkdir -p $(DIST_DIR)
	@./tools/package.sh live_day dev 2>&1 | tee $(LOGS_DIR)/package_live_day_dev.log
	@./tools/package.sh live_thermal dev 2>&1 | tee $(LOGS_DIR)/package_live_thermal_dev.log
	@./tools/package.sh recording_day dev 2>&1 | tee $(LOGS_DIR)/package_recording_day_dev.log
	@./tools/package.sh recording_thermal dev 2>&1 | tee $(LOGS_DIR)/package_recording_thermal_dev.log
	@echo ""
	@ls -lh $(DIST_DIR)/*-dev.tar 2>/dev/null || true
else
package-all-dev:
	@./tools/devcontainer-build.sh package-dev
endif

#==============================================================================
# Deploy Targets
#==============================================================================

# Load environment variables from .env if it exists
-include .env
export

ifeq ($(DEVCONTAINER),1)
# --- Devcontainer: Deploy dev builds ---
# Always clean-builds to ensure no stale artifacts
deploy: clean-artifacts package-all-dev
	@echo "=== Deploying dev packages ==="
	@./tools/deploy.sh dev 2>&1 | tee $(LOGS_DIR)/deploy_dev.log
	@echo "=== Deploy complete ==="

# Deploy production builds
# Always clean-builds to ensure no stale artifacts
deploy-prod: clean-artifacts package-all
	@echo "=== Deploying production packages ==="
	@./tools/deploy.sh production 2>&1 | tee $(LOGS_DIR)/deploy_prod.log
	@echo "=== Deploy complete ==="
else
# --- Host: Delegate to devcontainer script ---
deploy:
	@./tools/devcontainer-build.sh deploy

deploy-prod:
	@./tools/devcontainer-build.sh deploy-prod
endif

# Deploy only to frontend (live variants)
deploy-frontend: package-all-dev
	@echo "=== Deploying to frontend (dev) ==="
	@./tools/deploy.sh dev frontend 2>&1 | tee $(LOGS_DIR)/deploy_frontend.log

deploy-frontend-prod: package-all
	@echo "=== Deploying to frontend (production) ==="
	@./tools/deploy.sh production frontend 2>&1 | tee $(LOGS_DIR)/deploy_frontend_prod.log

# Deploy only to gallery (recording_day)
deploy-gallery: package-dev
	@echo "=== Deploying to gallery (dev) ==="
	@./tools/deploy.sh dev gallery 2>&1 | tee $(LOGS_DIR)/deploy_gallery.log

deploy-gallery-prod: package
	@echo "=== Deploying to gallery (production) ==="
	@./tools/deploy.sh production gallery 2>&1 | tee $(LOGS_DIR)/deploy_gallery_prod.log

#==============================================================================
# Test Harness Targets
#==============================================================================

harness: png-harness video-harness
	@echo "=== All test harnesses built ==="

png-harness:
	@echo "=== Building PNG harness ==="
	@mkdir -p $(BUILD_DIR)
	@$(NATIVE_CC) -o $(BUILD_DIR)/png_harness \
		$(PROJECT_ROOT)/test/osd_test.c \
		-I$(PROJECT_ROOT)/vendor \
		-I/usr/local/include \
		-L/usr/local/lib \
		-Wl,-rpath,/usr/local/lib \
		-lwasmtime -lpng -lm 2>&1 | tee $(LOGS_DIR)/build_png_harness.log
	@echo "Built: $(BUILD_DIR)/png_harness"

png: recording_day png-harness
	@echo "=== Generating PNG snapshot ==="
	@mkdir -p $(SNAPSHOT_DIR) $(DIST_DIR)
	@$(BUILD_DIR)/png_harness $(BUILD_DIR)/recording_day.wasm 2>&1 | tee $(LOGS_DIR)/png_recording_day.log
	@cp $(SNAPSHOT_DIR)/osd_render.png $(DIST_DIR)/recording_day.png 2>/dev/null || true
	@echo "Output: $(DIST_DIR)/recording_day.png"

ifeq ($(DEVCONTAINER),1)
png-all: all png-harness
	@echo "=== Generating all PNG snapshots ==="
	@mkdir -p $(SNAPSHOT_DIR) $(DIST_DIR)
	@for variant in live_day live_thermal recording_day recording_thermal; do \
		echo "Generating $$variant..."; \
		$(BUILD_DIR)/png_harness $(BUILD_DIR)/$$variant.wasm 2>&1 | tee $(LOGS_DIR)/png_$$variant.log && \
		cp $(SNAPSHOT_DIR)/osd_render.png $(DIST_DIR)/$$variant.png; \
	done
	@echo ""
	@ls -lh $(DIST_DIR)/*.png 2>/dev/null || true
else
png-all:
	@./tools/devcontainer-build.sh test-png
endif

video-harness:
	@echo "=== Building video harness ==="
	@$(MAKE) -C test/video_harness clean all BUILD_MODE=production

ifeq ($(DEVCONTAINER),1)
video: all video-harness
	@echo "=== Generating videos for all 4 variants ==="
	@mkdir -p test/output
	@$(MAKE) -C test/video_harness run BUILD_MODE=production
	@echo ""
	@ls -lh test/output/*.mp4 2>/dev/null || echo "No video files generated"
else
video:
	@./tools/devcontainer-build.sh test-video
endif

video-all: all-modes video-harness
	@echo "=== Generating videos for all variants (production only) ==="
	@mkdir -p test/output
	@for variant in live_day live_thermal recording_day recording_thermal; do \
		echo "Generating video: $$variant..."; \
		cd $(PROJECT_ROOT)/test/video_harness && ./video_harness $$variant 2>&1 | tee $(LOGS_DIR)/video_$$variant.log; \
	done
	@echo ""
	@ls -lh test/output/*.mp4 2>/dev/null || echo "No video files generated"

#==============================================================================
# Proto Targets
#==============================================================================

ifeq ($(DEVCONTAINER),1)
proto:
	@echo "=== Updating proto submodules ==="
	git submodule update --remote --merge proto/c proto/ts
	@echo "=== Syncing proto/c to src/proto ==="
	@cp proto/c/*.pb.h proto/c/*.pb.c src/proto/
	@echo "=== Syncing proto/c/opaque to src/proto/opaque ==="
	@mkdir -p src/proto/opaque
	@cp proto/c/opaque/*.pb.h proto/c/opaque/*.pb.c src/proto/opaque/ 2>/dev/null || true
	@echo "✅ Proto files updated from submodules"
else
proto:
	@./tools/devcontainer-build.sh proto
endif

#==============================================================================
# CI Targets (Full Pipeline)
#==============================================================================

ifeq ($(DEVCONTAINER),1)
# Build all 8 variants (4 production + 4 dev) in parallel
# Clean WASM files first to ensure no stale binaries can be packaged
all-modes: quality
	@echo "=== Building all 8 variants ($(JOBS) parallel jobs) ==="
	@rm -f $(BUILD_DIR)/*.wasm
	@$(MAKE) -j$(JOBS) --no-print-directory \
		_live_day _live_thermal _recording_day _recording_thermal \
		_live_day_dev _live_thermal_dev _recording_day_dev _recording_thermal_dev \
		BUILD_MODE=production
	@echo ""
	@echo "=== All 8 WASM variants built ==="
	@ls -lh $(BUILD_DIR)/*.wasm 2>/dev/null || true
else
all-modes:
	@./tools/devcontainer-build.sh wasm-all
endif

png-all-modes: all-modes png-harness
	@echo "=== Generating PNG snapshots for all variants (both modes) ==="
	@mkdir -p $(SNAPSHOT_DIR) $(DIST_DIR)
	@for mode in production dev; do \
		suffix=""; \
		[ "$$mode" = "dev" ] && suffix="_dev"; \
		for variant in live_day live_thermal recording_day recording_thermal; do \
			echo "Generating PNG: $${variant}$${suffix}..."; \
			$(BUILD_DIR)/png_harness $(BUILD_DIR)/$${variant}$${suffix}.wasm 2>&1 | tee $(LOGS_DIR)/png_$${variant}_$${mode}.log && \
			cp $(SNAPSHOT_DIR)/osd_render.png $(DIST_DIR)/$${variant}$${suffix}.png; \
		done; \
	done
	@echo ""
	@ls -lh $(DIST_DIR)/*.png 2>/dev/null || true

ifeq ($(DEVCONTAINER),1)
ci: proto all-modes png-all-modes video-all
	@echo ""
	@echo "=============================================="
	@echo "CI BUILD COMPLETE"
	@echo "=============================================="
	@echo "Built:     8 WASM variants (4 variants x 2 modes)"
	@echo "Generated: 8 PNG snapshots"
	@echo "Generated: 4 videos"
	@echo ""
	@echo "WASM files:"
	@ls -lh $(BUILD_DIR)/*.wasm 2>/dev/null || true
	@echo ""
	@echo "PNG files:"
	@ls -lh $(DIST_DIR)/*.png 2>/dev/null || true
	@echo ""
	@echo "Video files:"
	@ls -lh test/output/*.mp4 2>/dev/null || true
else
ci:
	@./tools/devcontainer-build.sh ci
endif

#==============================================================================
# Clean
#==============================================================================

# Clean WASM binaries only (keeps .o files for faster rebuilds)
clean-wasm:
	@echo "=== Cleaning WASM binaries ==="
	rm -f $(BUILD_DIR)/*.wasm
	@echo "Cleaned: build/*.wasm"

# Clean packages only (useful before manual packaging)
clean-packages:
	@echo "=== Cleaning packages ==="
	rm -f $(DIST_DIR)/*.tar
	rm -f $(DIST_DIR)/*.png
	@echo "Cleaned: dist/*.tar dist/*.png"

# Clean deployable artifacts (WASM + packages, keeps .o files)
clean-artifacts: clean-wasm clean-packages
	@echo "Cleaned all deployable artifacts"

# Full clean (removes everything including .o files)
clean:
	@echo "=== Full clean ==="
	rm -rf $(BUILD_DIR)
	rm -rf $(DIST_DIR)
	rm -rf $(SNAPSHOT_DIR)
	rm -rf $(LOGS_DIR)
	@echo "Cleaned: build/ dist/ snapshot/ logs/"

#==============================================================================
# Help
#==============================================================================

help:
	@echo "Jettison WASM OSD - Build System"
	@echo ""
	@echo "Build Environment: $(BUILD_ENV)"
ifeq ($(DEVCONTAINER),1)
	@echo "  Running inside devcontainer - direct compilation"
else
	@echo "  Running on host - delegates to ./tools/devcontainer-build.sh"
endif
	@echo ""
	@echo "Usage: make [target] [VARIANT=name] [BUILD_MODE=mode]"
	@echo ""
	@echo "Main Targets:"
	@echo "  make              Build recording_day (default)"
	@echo "  make all          Build all 4 variants"
	@echo "  make package      Build + package recording_day"
	@echo "  make package-all  Build + package all variants"
	@echo ""
	@echo "Clean Targets:"
	@echo "  make clean           Full clean (removes everything)"
	@echo "  make clean-wasm      Clean WASM binaries only (keeps .o files)"
	@echo "  make clean-packages  Clean packages and PNGs only"
	@echo "  make clean-artifacts Clean WASM + packages (keeps .o files)"
	@echo ""
	@echo "Deploy Targets:"
	@echo "  make deploy            Dev build + deploy (frontend + gallery)"
	@echo "  make deploy-prod       Prod build + deploy (frontend + gallery)"
	@echo "  make deploy-frontend   Dev build + deploy live variants only"
	@echo "  make deploy-gallery    Dev build + deploy recording_day only"
	@echo ""
	@echo "CI/Full Pipeline:"
	@echo "  make ci           Full CI build (8 WASM + PNGs + videos)"
	@echo "  make all-modes    Build all 4 variants in both modes (8 WASM)"
	@echo "  make proto        Update proto submodules (proto/c, proto/ts)"
	@echo ""
	@echo "Test Outputs:"
	@echo "  make png          Generate PNG snapshot (recording_day)"
	@echo "  make png-all      Generate PNG snapshots (4 variants)"
	@echo "  make png-all-modes Generate PNG snapshots (8 variants)"
	@echo "  make video        Generate videos (4 variants)"
	@echo "  make video-all    Generate videos (4 variants, production)"
	@echo ""
	@echo "Test Harnesses:"
	@echo "  make harness      Build all harnesses (png + video)"
	@echo "  make png-harness  Build PNG harness only"
	@echo "  make video-harness Build video harness only"
	@echo ""
	@echo "Individual Variants:"
	@echo "  recording_day        Recording + Day (1920x1080)"
	@echo "  recording_thermal    Recording + Thermal (900x720)"
	@echo "  live_day             Live + Day (1920x1080)"
	@echo "  live_thermal         Live + Thermal (900x720)"
	@echo ""
	@echo "Quality (runs automatically):"
	@echo "  make format       Run clang-format"
	@echo "  make lint         Run clang-tidy"
	@echo "  make quality      Run format + lint"
	@echo ""
	@echo "IDE Support:"
	@echo "  make index        Generate compile_commands.json (CLion/clangd)"
	@echo ""
	@echo "Options:"
	@echo "  BUILD_MODE=production  Optimized (default, ~640KB)"
	@echo "  BUILD_MODE=dev         Debug + hardening (~2.9MB)"
	@echo "  JOBS=N                 Parallel jobs for multi-variant builds (default: 8)"
	@echo ""
	@echo "Output Directories:"
	@echo "  build/            WASM binaries"
	@echo "  dist/             Packages and PNG snapshots"
	@echo "  test/output/      Video files"
	@echo "  logs/             Build logs"
