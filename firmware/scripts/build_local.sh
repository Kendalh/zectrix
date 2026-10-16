#!/usr/bin/env bash
# Build the Zectrix S3 e-paper 4.2 firmware with a locally installed ESP-IDF and collect the .bin files.
# Mirrors scripts/release.py (which needs a main/boards/*/config.json that this repo does not ship).
#
#   firmware/scripts/build_local.sh [--clean]
#
# Environment:
#   IDF_PATH      ESP-IDF checkout (default ~/esp/esp-idf). Must match dependencies.lock, see below.
#   LIBUSB_DIR    private libusb for openocd, used if present (default ~/esp/libusb-pkg/extracted/usr/lib/x86_64-linux-gnu)
#   FORCE=1       skip the ESP-IDF version check
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
BOARD_TYPE="zectrix-s3-epaper-4.2"
CLEAN=0
[[ "${1:-}" == "--clean" ]] && CLEAN=1

# The ESP-IDF version recorded in dependencies.lock is the one the components were resolved with.
# v6.0.3 fails to build (esp_cam_sensor uses SPI internals that changed in a 6.0.x patch release).
LOCK_VER="$(awk '/^  idf:/{f=1} f&&/version:/{print $2; exit}' "$FIRMWARE_DIR/dependencies.lock")"
WANT_TAG="v${LOCK_VER%.0}"
HAVE_TAG="$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo unknown)"
if [[ "$HAVE_TAG" != "$WANT_TAG" && "${FORCE:-0}" != "1" ]]; then
  echo "[ERROR] ESP-IDF at $IDF_PATH is $HAVE_TAG, dependencies.lock needs $WANT_TAG (FORCE=1 to ignore)" >&2
  exit 1
fi

# Use the system Python: a project venv earlier in PATH breaks ESP-IDF's own Python environment.
export PATH=/usr/bin:/usr/local/bin:/bin
unset IDF_TARGET
LIBUSB_DIR="${LIBUSB_DIR:-$HOME/esp/libusb-pkg/extracted/usr/lib/x86_64-linux-gnu}"
# openocd (required by export.sh) needs libusb-1.0; a private copy avoids needing root.
[[ -d "$LIBUSB_DIR" ]] && export LD_LIBRARY_PATH="$LIBUSB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null

cd "$FIRMWARE_DIR"
if [[ $CLEAN -eq 1 ]]; then rm -rf build sdkconfig sdkconfig.old; fi
if [[ ! -f sdkconfig ]]; then
  idf.py set-target esp32s3
  printf '\n# Append by build_local.sh\nCONFIG_BOARD_TYPE_ZECTRIX_S3_EPAPER_4_2=y\n' >> sdkconfig
fi
idf.py -DBOARD_NAME="$BOARD_TYPE" -DBOARD_TYPE="$BOARD_TYPE" build
idf.py merge-bin

VER="$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)"
OUT="releases/v${VER}_${BOARD_TYPE}"
mkdir -p "$OUT"
cp build/merged-binary.bin "$OUT/merged-binary.bin"
cp build/xiaozhi.bin "$OUT/app-only_xiaozhi.bin"
touch -r build/merged-binary.bin "$OUT/merged-binary.bin" "$OUT/app-only_xiaozhi.bin"  # keep build time as mtime
(cd "$OUT" && sha256sum merged-binary.bin app-only_xiaozhi.bin > SHA256SUMS)
{
  echo "Board:       $BOARD_TYPE (ESP32-S3, 16MB flash, 8MB PSRAM, 4-color e-paper)"
  echo "Firmware:    PROJECT_VER $VER"
  echo "Git commit:  $(git rev-parse --short HEAD) ($(git branch --show-current)), working tree clean: $([ -z "$(git status --porcelain -- . ':!releases')" ] && echo yes || echo no)"
  echo "ESP-IDF:     $HAVE_TAG"
  echo "Built:       $(date -r build/merged-binary.bin '+%Y-%m-%d %H:%M:%S')"
  echo
  echo "merged-binary.bin    full image (bootloader + partition table + otadata + app): flash at 0x0"
  echo "app-only_xiaozhi.bin app image only: flash at 0x20000 (ota_0)"
  echo "Not included: the 8MB 'assets' SPIFFS partition at 0x800000 (photo storage)."
} > "$OUT/BUILD_INFO.txt"

if ! git diff --quiet -- dependencies.lock; then
  echo "[WARN] dependencies.lock was modified by the build; review it, do not commit it by accident" >&2
fi
echo "[OK] $OUT"
ls -l "$OUT"
