#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
WORKSPACE=$(realpath "${1:-$REPO_ROOT/..}")

NUTTX_DIR="$WORKSPACE/nuttx"
APPS_DIR="$WORKSPACE/apps"
VENDOR_DIR="$WORKSPACE/vendor/artinchip"
LVGL_DIR="$APPS_DIR/graphics/lvgl"
LVGL_ARCHIVE="$LVGL_DIR/v9.2.1.zip"

if [[ ! -f "$NUTTX_DIR/Makefile" ]]; then
  echo "error: NuttX tree not found at $NUTTX_DIR" >&2
  exit 1
fi

if [[ ! -d "$VENDOR_DIR/tools/scripts" ]]; then
  echo "error: vendor_artinchip tree not found at $VENDOR_DIR" >&2
  exit 1
fi

if [[ ! -f "$LVGL_DIR/lvgl/Kconfig" ]]; then
  if [[ -f "$WORKSPACE/lvgl-9.2.1.zip" ]]; then
    cp -f "$WORKSPACE/lvgl-9.2.1.zip" "$LVGL_ARCHIVE"
  elif [[ ! -f "$LVGL_ARCHIVE" ]]; then
    curl -L --fail --silent --show-error \
      -o "$LVGL_ARCHIVE" \
      https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.1.zip
  fi

  unzip -oq "$LVGL_ARCHIVE" -d "$LVGL_DIR"
  mv "$LVGL_DIR/lvgl-9.2.1" "$LVGL_DIR/lvgl"
fi

if ! grep -q '^#include <unistd.h>$' \
  "$LVGL_DIR/lvgl/src/drivers/nuttx/lv_nuttx_image_cache.c"; then
  patch -d "$LVGL_DIR/lvgl" -p1 \
    < "$REPO_ROOT/patches/lvgl-9.2.1-nuttx-gettid.patch"
fi

if ! grep -q 'CONFIG_LV_MEM_SIZE_KILOBYTES' \
  "$LVGL_DIR/lvgl/src/lv_conf_internal.h"; then
  patch -d "$LVGL_DIR/lvgl" -p1 \
    < "$REPO_ROOT/patches/lvgl-9.2.1-nuttx-memory-kconfig.patch"
fi

# Match the LVGL wrapper target convention so make does not unpack the
# archive again and overwrite the NuttX compatibility patch.
touch "$LVGL_DIR/lvgl"

install -d \
  "$APPS_DIR/industry/d13x_home_panel" \
  "$VENDOR_DIR/boards/d13x-hengshan-pi" \
  "$VENDOR_DIR/boards/d13x/hengshan-pi/pack" \
  "$VENDOR_DIR/chips/d13x" \
  "$VENDOR_DIR/pack/resources/font" \
  "$VENDOR_DIR/pack/prebuilt"

cp -a "$REPO_ROOT/board/d13x-hengshan-pi/." \
  "$VENDOR_DIR/boards/d13x-hengshan-pi/"
cp -a "$REPO_ROOT/app/home_panel/." \
  "$APPS_DIR/industry/d13x_home_panel/"
(cd "$APPS_DIR/industry" && \
  "$APPS_DIR/tools/mkkconfig.sh" -m "Industrial Applications")
cp -a "$REPO_ROOT/board/d13x-hengshan-pi/pack/." \
  "$VENDOR_DIR/boards/d13x/hengshan-pi/pack/"
cp -a "$REPO_ROOT/chip/d13x/." "$VENDOR_DIR/chips/d13x/"
cp -a "$REPO_ROOT/nuttx-overlay/." "$NUTTX_DIR/"
cp -a "$REPO_ROOT/vendor-overlay/." "$VENDOR_DIR/"
cp -f "$REPO_ROOT/assets/fonts/MiSans-Regular-18-full.bin" \
  "$VENDOR_DIR/pack/resources/font/MiSans-Regular-18-full.bin"

echo "D13x contest sources integrated into $WORKSPACE"
