#!/bin/bash

set -e

CUR_DIR=$(pwd)
SDK_ROOT=$(realpath "$CUR_DIR/../../..")
NUTTX_CONFIG="$SDK_ROOT/nuttx/.config"

# 1.  auto from .config or pass as parameter (e.g., demo68-nor)
BOARD_NAME=$1
if [ -z "$BOARD_NAME" ]; then
    if [ -f "$NUTTX_CONFIG" ]; then
        BOARD_DIR_CONFIG=$(grep '^CONFIG_ARCH_BOARD_CUSTOM_DIR=' "$NUTTX_CONFIG" | cut -d'=' -f2- | tr -d '"')
        BOARD_NAME=$(basename "$BOARD_DIR_CONFIG")
    fi
fi
#  get chip model (e.g., d12x)
CHIP_NAME=$2
if [ -z "$CHIP_NAME" ]; then
    if [ -f "$NUTTX_CONFIG" ]; then
        CHIP_NAME=$(grep '^CONFIG_ARCH_CHIP_CUSTOM_NAME=' "$NUTTX_CONFIG" | cut -d'=' -f2- | tr -d '"')
        if [ -n "$CHIP_NAME" ]; then
            :
        elif grep -q "CONFIG_ARCH_CHIP_D12X=y" "$NUTTX_CONFIG"; then
            CHIP_NAME="d12x"
        else
            CHIP_NAME=$(grep -E "CONFIG_ARCH_CHIP_[A-Z0-9]+" $SDK_ROOT/nuttx/.config | grep "=y" | head -n 1 | cut -d'_' -f4 | tr '[:upper:]' '[:lower:]' | cut -d'=' -f1)
        fi
    fi
fi

#  default fallback
BOARD_NAME=${BOARD_NAME:-demo68-nor}
CHIP_NAME=${CHIP_NAME:-d12x}
BOARD_NAME=${BOARD_NAME#"$CHIP_NAME"-}

echo ">>> Packing for Board: $BOARD_NAME, Chip: $CHIP_NAME"

BOARD_DIR="$SDK_ROOT/vendor/artinchip/boards/$CHIP_NAME/$BOARD_NAME"
PRJ_OUT=$(realpath "$CUR_DIR/prebuilt")
TOOLDIR=$SDK_ROOT/vendor/artinchip/tools/scripts
TOOLCHAIN_DIR=${SDK_ROOT}/vendor/artinchip/toolchain/bin/riscv64-unknown-elf-
OBJ_COPY=${TOOLCHAIN_DIR}objcopy
ELF_SIZE=${TOOLCHAIN_DIR}size
if [ ! -x "$OBJ_COPY" ]; then
    OBJ_COPY=$(command -v riscv32-unknown-elf-objcopy || \
        command -v riscv-none-elf-objcopy || true)
fi
if [ ! -x "$ELF_SIZE" ]; then
    ELF_SIZE=$(command -v riscv32-unknown-elf-size || \
        command -v riscv-none-elf-size || \
        command -v riscv64-unknown-elf-size || true)
fi

if [ -z "$OBJ_COPY" ]; then
    echo "Error: RISC-V objcopy not found" >&2
    exit 1
fi

echo ">>> Starting $CHIP_NAME image packing..."

# 2.  sync from $BOARD_DIR/pack to $PRJ_OUT/ from boards directory
if [ -d "$BOARD_DIR/pack" ]; then
    echo ">>> Syncing board specific pack config from $BOARD_DIR/pack"
    cp -rv "$BOARD_DIR/pack/"* "$PRJ_OUT/"
else
    echo "Error: board pack configuration not found: $BOARD_DIR/pack" >&2
    exit 1
fi

# Keep filesystem image sizes in sync with the board partition layout.
python3 "$TOOLDIR/gen_partition_file_list.py" \
    -c "$PRJ_OUT/image_cfg.json" \
    -o "$PRJ_OUT/partition_file_list.h"

# 3.  prepare base files (ELF/Manifest) and convert to binary nuttx.bin
NUTTX_ELF="$SDK_ROOT/nuttx/nuttx.elf"
if [ ! -f "$NUTTX_ELF" ]; then
    NUTTX_ELF="$SDK_ROOT/nuttx/nuttx"
fi

if [ -f "$NUTTX_ELF" ]; then
    cp -f "$NUTTX_ELF" "$PRJ_OUT/$CHIP_NAME.elf"
    if [ -f "$SDK_ROOT/nuttx/nuttx.manifest" ]; then
        cp -f "$SDK_ROOT/nuttx/nuttx.manifest" "$PRJ_OUT/nuttx.manifest"
    fi

    #  generate nuttx.bin from latest ELF file
    echo ">>> Generating nuttx.bin from $CHIP_NAME.elf..."
    "$OBJ_COPY" -S -O binary -R .note -R .note.gnu.build-id -R .comment "$PRJ_OUT/$CHIP_NAME.elf" "$PRJ_OUT/nuttx.bin"

    #  display ELF file size on console
    if [ -n "$ELF_SIZE" ]; then
        "$ELF_SIZE" "$PRJ_OUT/$CHIP_NAME.elf"
    fi
else
    echo "Error: NuttX ELF not found" >&2
    exit 1
fi

# 4.  prepare file system content (SDK resource installation)
python3 $TOOLDIR/fsinstall.py --sdkout $PRJ_OUT --clean rodata/,data/
python3 $TOOLDIR/fsinstall.py --sdkout $PRJ_OUT --src $SDK_ROOT/vendor/artinchip/pack/resources/rodata/ --dst rodata/
python3 $TOOLDIR/fsinstall.py --sdkout $PRJ_OUT --src $SDK_ROOT/vendor/artinchip/pack/resources/data/ --dst data/

FONT_SOURCE="$SDK_ROOT/vendor/artinchip/pack/resources/font/MiSans-Regular-18-full.bin"
FONT_TARGET="$PRJ_OUT/MiSans-Regular-18-full.bin"
FONT_LIMIT=$((8 * 1024 * 1024))
if [ ! -f "$FONT_SOURCE" ]; then
    echo "Error: precompiled full MiSans font not found: $FONT_SOURCE" >&2
    exit 1
fi
FONT_SIZE=$(stat -c %s "$FONT_SOURCE")
if [ "$FONT_SIZE" -gt "$FONT_LIMIT" ]; then
    echo "Error: MiSans font ($FONT_SIZE bytes) exceeds 8 MiB partition" >&2
    exit 1
fi
cp -f "$FONT_SOURCE" "$FONT_TARGET"

# 6.  generate file system image (FATFS and LittleFS)
pushd $PRJ_OUT > /dev/null
if grep -q '"rodata"[[:space:]]*:' "$PRJ_OUT/image_cfg.json"; then
    if ! python3 $TOOLDIR/makefatfs.py --fullpart --volab default --cluster 8 --sector 512 --tooldir $TOOLDIR --inputdir rodata --outfile $PRJ_OUT/rodata.fatfs; then
        echo ">>> Sparse FAT conversion unavailable; validating raw FAT image"
        FAT_CHECK_DIR=$(mktemp -d)
        trap 'rm -rf "$FAT_CHECK_DIR"' EXIT
        $TOOLDIR/mcopy -i $PRJ_OUT/rodata.fatfs -s '::/*' "$FAT_CHECK_DIR/"
        diff -qr "$PRJ_OUT/rodata" "$FAT_CHECK_DIR"
        rm -rf "$FAT_CHECK_DIR"
        trap - EXIT
    fi
fi
python3 $TOOLDIR/makelittlefs.py --pagesize 256 --blocksize 4096 --tooldir $TOOLDIR --inputdir data/ --outfile $PRJ_OUT/data.lfs
popd > /dev/null

# 7.  generate private resources (PBP/Partition combination)
python3 $TOOLDIR/mk_private_resource.py -v -l $PRJ_OUT/pbp_cfg.json,$PRJ_OUT/partition.json -o $PRJ_OUT/pbp_cfg.bin

# 8.  generate final .img image
IMG_VERSION=$(sed -n \
    's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$PRJ_OUT/image_cfg.json" | head -n 1)
if [ -z "$IMG_VERSION" ]; then
    echo "Error: image version is missing from image_cfg.json" >&2
    exit 1
fi

IMG_NAME="${CHIP_NAME}_${BOARD_NAME}_v${IMG_VERSION}.img"
python3 $TOOLDIR/mk_image.py -v -c $PRJ_OUT/image_cfg.json -d $PRJ_OUT

echo ">>> Image generated successfully at: $PRJ_OUT/$IMG_NAME"
