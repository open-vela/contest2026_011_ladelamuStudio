#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
WORKSPACE=$(realpath "${1:-$REPO_ROOT/..}")
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}
export PATH="$REPO_ROOT/scripts/tools:$PATH"

VERSION=$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "error: invalid four-part version: $VERSION" >&2
  exit 1
fi

if ! grep -q "\"version\": \"$VERSION\"" \
  "$REPO_ROOT/board/d13x-hengshan-pi/pack/image_cfg.json"; then
  echo "error: VERSION and image_cfg.json do not match" >&2
  exit 1
fi

if ! grep -q "version = \"$VERSION\";" \
  "$REPO_ROOT/board/d13x-hengshan-pi/pack/d13x_os.its"; then
  echo "error: VERSION and d13x_os.its do not match" >&2
  exit 1
fi

"$SCRIPT_DIR/integrate.sh" "$WORKSPACE"

cd "$WORKSPACE/nuttx"
export APPSDIR="$WORKSPACE/apps"
export APPSBINDIR="$WORKSPACE/apps"
export BINDIR="$WORKSPACE/nuttx"
./tools/configure.sh \
  ../vendor/artinchip/boards/d13x-hengshan-pi/configs/nsh
make -j"$JOBS"

# The application executes from the 8 MiB PSRAM initialized by PBP.  Validate
# both the ELF entry and every loadable segment before constructing the FIT.

PSRAM_BASE=$((0x40000000))
PSRAM_END=$((0x40800000))
ELF="$WORKSPACE/nuttx/nuttx"
ENTRY=$(readelf -h "$ELF" |
  awk '/Entry point address:/ { print $4 }')

if ((ENTRY != PSRAM_BASE)); then
  printf 'error: NuttX entry is %s, expected 0x%08x\n' \
    "${ENTRY:-unknown}" "$PSRAM_BASE" >&2
  exit 1
fi

while read -r vaddr filesz memsz; do
  segment_start=$((vaddr))
  segment_file_end=$((vaddr + filesz))
  segment_mem_end=$((vaddr + memsz))
  if ((segment_start < PSRAM_BASE || segment_file_end > PSRAM_END ||
       segment_mem_end > PSRAM_END)); then
    printf 'error: LOAD segment %s..0x%x (memory 0x%x) is outside PSRAM\n' \
      "$vaddr" "$segment_file_end" "$segment_mem_end" >&2
    exit 1
  fi
done < <(readelf -W -l "$ELF" |
  awk '$1 == "LOAD" { print $3, $5, $6 }')

LOAD_BYTES=$(riscv32-unknown-elf-size "$ELF" |
  awk 'NR == 2 { print $1 + $2 }')
echo "NuttX PSRAM load image: $LOAD_BYTES bytes; entry=$ENTRY"

cd "$WORKSPACE/vendor/artinchip/pack"
./pack.sh hengshan-pi d13x

IMAGE="$WORKSPACE/vendor/artinchip/pack/prebuilt/d13x_hengshan-pi_v$VERSION.img"
test -s "$IMAGE"
sha256sum "$WORKSPACE/nuttx/nuttx" "$IMAGE"
