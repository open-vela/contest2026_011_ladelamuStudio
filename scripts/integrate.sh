#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
WORKSPACE=${1:-$(cd "$REPO_ROOT/.." && pwd)}

NUTTX_DIR="$WORKSPACE/nuttx"
VENDOR_DIR="$WORKSPACE/vendor/artinchip"

if [[ ! -f "$NUTTX_DIR/Makefile" ]]; then
  echo "error: NuttX tree not found at $NUTTX_DIR" >&2
  exit 1
fi

if [[ ! -d "$VENDOR_DIR/tools/scripts" ]]; then
  echo "error: vendor_artinchip tree not found at $VENDOR_DIR" >&2
  exit 1
fi

install -d \
  "$VENDOR_DIR/boards/d13x-hengshan-pi" \
  "$VENDOR_DIR/boards/d13x/hengshan-pi/pack" \
  "$VENDOR_DIR/chips/d13x" \
  "$VENDOR_DIR/pack/prebuilt"

cp -a "$REPO_ROOT/board/d13x-hengshan-pi/." \
  "$VENDOR_DIR/boards/d13x-hengshan-pi/"
cp -a "$REPO_ROOT/board/d13x-hengshan-pi/pack/." \
  "$VENDOR_DIR/boards/d13x/hengshan-pi/pack/"
cp -a "$REPO_ROOT/chip/d13x/." "$VENDOR_DIR/chips/d13x/"
cp -a "$REPO_ROOT/nuttx-overlay/." "$NUTTX_DIR/"
cp -a "$REPO_ROOT/vendor-overlay/." "$VENDOR_DIR/"

echo "D13x contest sources integrated into $WORKSPACE"
