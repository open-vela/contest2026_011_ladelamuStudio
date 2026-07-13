#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
WORKSPACE=${1:-$(cd "$REPO_ROOT/.." && pwd)}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}
export PATH="$REPO_ROOT/scripts/tools:$PATH"

"$SCRIPT_DIR/integrate.sh" "$WORKSPACE"

cd "$WORKSPACE/nuttx"
export APPSDIR="$WORKSPACE/apps"
export APPSBINDIR="$WORKSPACE/apps"
export BINDIR="$WORKSPACE/nuttx"
./tools/configure.sh \
  ../vendor/artinchip/boards/d13x-hengshan-pi/configs/nsh
make -j"$JOBS"

cd "$WORKSPACE/vendor/artinchip/pack"
./pack.sh hengshan-pi d13x

IMAGE="$WORKSPACE/vendor/artinchip/pack/prebuilt/d13x_hengshan-pi_v1.0.0.img"
test -s "$IMAGE"
sha256sum "$WORKSPACE/nuttx/nuttx" "$IMAGE"
