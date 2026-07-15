# D13x 衡山派新电脑编译指南

本文面向第一次在新电脑上编译本项目的开发者。推荐直接克隆完整工作区仓库，
它通过 Git submodule 固定了 NuttX、OpenVela apps、ArtInChip vendor 和本比赛仓
的已知可构建版本。

## 1. 主机要求

- 64 位 x86 Linux。CI 使用 Ubuntu；Ubuntu/Debian 和 Arch Linux 均可。
- 至少 8 GiB 内存、15 GiB 可用磁盘空间。
- 首次构建需要访问 GitHub，以下载子模块、LVGL 9.2.1 和 cJSON 1.7.12。
- 不要使用 `sudo` 运行构建脚本，否则会在源码目录中留下 root 所有者文件。

Windows 用户建议使用原生 Linux 或 Linux 虚拟机。WSL2 可以编译，但烧录工具和
USB 串口需要另外配置 USB 透传。

## 2. 安装主机依赖

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y \
  build-essential ccache cmake flex bison gperf genromfs libselinux1 \
  ninja-build python3 python3-pip python3-venv wget curl unzip patch \
  git file
```

### Arch Linux

```bash
sudo pacman -Syu
sudo pacman -S --needed \
  base-devel ccache cmake flex bison gperf genromfs libselinux ninja \
  python python-pip wget curl unzip patch git file
```

为避免污染系统 Python，建议建立一次专用虚拟环境：

```bash
python3 -m venv "$HOME/.venvs/openvela-d13x"
source "$HOME/.venvs/openvela-d13x/bin/activate"
python3 -m pip install --upgrade pip
python3 -m pip install kconfiglib pycryptodome pyelftools pyyaml
```

以后打开新终端编译前，重新执行：

```bash
source "$HOME/.venvs/openvela-d13x/bin/activate"
```

## 3. 安装 RISC-V 裸机工具链

项目 CI 使用 xPack GNU RISC-V Embedded GCC 14.2.0-3。以下命令将工具链安装到
当前用户目录，不会修改系统目录：

```bash
mkdir -p "$HOME/.local/opt" "$HOME/.local/bin"
cd /tmp
wget -O riscv-toolchain.tar.gz \
  https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v14.2.0-3/xpack-riscv-none-elf-gcc-14.2.0-3-linux-x64.tar.gz
tar -xzf riscv-toolchain.tar.gz -C "$HOME/.local/opt"

toolchain="$HOME/.local/opt/xpack-riscv-none-elf-gcc-14.2.0-3/bin"
for tool in ar gcc g++ ld nm objcopy objdump ranlib readelf size strip; do
  ln -sf "$toolchain/riscv-none-elf-$tool" \
    "$HOME/.local/bin/riscv32-unknown-elf-$tool"
done
```

将工具链永久加入 `PATH`：

```bash
cat >> "$HOME/.bashrc" <<'EOF'
export PATH="$HOME/.local/bin:$HOME/.local/opt/xpack-riscv-none-elf-gcc-14.2.0-3/bin:$PATH"
EOF
source "$HOME/.bashrc"
```

确认两个命令前缀都可用：

```bash
riscv-none-elf-gcc --version
riscv32-unknown-elf-gcc --version
riscv32-unknown-elf-size --version
```

第一条应显示 `14.2.0`。构建配置使用 `riscv32-unknown-elf-*`，软链接则指向同一套
xPack 工具链，不能省略。

## 4. 获取完整源码

推荐克隆完整工作区，而不是分别猜测各仓库版本：

```bash
cd "$HOME"
git clone --recurse-submodules \
  https://github.com/lladlam/2026OpenVela.git OpenVela
cd OpenVela
git submodule update --init --recursive
```

目录应包含：

```text
OpenVela/
├── build.sh
├── contest2026_011_ladelamuStudio/
└── openvela/
    ├── apps/
    ├── nuttx/
    └── vendor/artinchip/
```

检查子模块是否完整：

```bash
git submodule status --recursive
test -f openvela/nuttx/Makefile
test -d openvela/apps
test -d openvela/vendor/artinchip/tools/scripts
```

`git submodule status` 中任何一行以 `-` 开头都表示该子模块尚未拉取，应再次执行
`git submodule update --init --recursive`。

## 5. 编译并打包

在完整工作区根目录执行：

```bash
cd "$HOME/OpenVela"
source "$HOME/.venvs/openvela-d13x/bin/activate"
./build.sh build
```

也可以限制并行任务数，内存较小的电脑建议使用 2：

```bash
JOBS=2 ./build.sh build
```

脚本会自动完成以下步骤：

1. 将比赛仓中的 D13x 芯片、板级和家庭屏源码集成到 OpenVela。
2. 获取并集成固定版本 LVGL 9.2.1。
3. 生成 `d13x-hengshan-pi:nsh` 配置并编译 NuttX。
4. 检查 ELF 入口和所有 LOAD 段均位于 8 MiB PSRAM。
5. 调用 ArtInChip 打包工具生成 AiBurn 可烧录镜像。

成功时终端最后会打印 ELF 和镜像的 SHA-256。产物位于：

```text
openvela/nuttx/nuttx
openvela/vendor/artinchip/pack/prebuilt/d13x_hengshan-pi_v<VERSION>.img
```

当前版本号以
`contest2026_011_ladelamuStudio/VERSION` 为准，不要在命令或脚本中写死版本号。

## 6. 清理与重新编译

普通源码修改后直接再次执行 `./build.sh build` 即可。配置变化较大或遇到旧目标
文件干扰时，执行完整清理：

```bash
cd "$HOME/OpenVela"
./build.sh clean
./build.sh build
```

`clean` 只清理 NuttX 构建配置和目标文件，不会删除源码或 Git 子模块。

## 7. 仅克隆比赛仓时的构建方式

如果已经按照 OpenVela 官方流程取得一个平铺工作区：

```text
<workspace>/
├── apps/
├── nuttx/
├── vendor/artinchip/
└── contest2026_011_ladelamuStudio/
```

可直接执行：

```bash
cd <workspace>/contest2026_011_ladelamuStudio
./scripts/build.sh <workspace>
```

完整工作区仓库采用 `openvela/` 子目录布局，因此应优先使用根目录的
`./build.sh build`，不要把仓库根目录错误地传给 `scripts/build.sh`。

## 8. 常见问题

### `riscv32-unknown-elf-gcc: command not found`

重新加载 `~/.bashrc`，并检查软链接：

```bash
source "$HOME/.bashrc"
ls -l "$HOME/.local/bin/riscv32-unknown-elf-gcc"
command -v riscv32-unknown-elf-gcc
```

### 找不到 `nuttx/Makefile` 或 `vendor/artinchip/tools/scripts`

子模块未完整拉取，回到工作区根目录执行：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

### LVGL 或 cJSON 下载失败

首次构建需要联网。确认可以访问 GitHub 后重新执行构建；脚本使用固定版本，下载
成功后会复用本地文件。代理环境需要同时为 Git、`curl` 和 `wget` 配置代理。

### `img2simg` 提示缺少 `libselinux.so.1`

Ubuntu/Debian 安装 `libselinux1`，Arch 安装 `libselinux`。如果日志随后明确显示
`Sparse FAT conversion unavailable; validating raw FAT image` 并且最终生成了镜像，
构建脚本已经使用经过校验的 raw FAT 回退路径，镜像仍可用于烧录。

### 编译进程被 `Killed`

通常是内存不足。关闭其他大型程序后执行：

```bash
JOBS=2 ./build.sh build
```

仍然失败时可改为 `JOBS=1`。

### 目录中出现 root 所有者文件

不要使用 `sudo ./build.sh`。修复现有目录权限后重新构建：

```bash
sudo chown -R "$USER:$USER" "$HOME/OpenVela"
```

## 9. 烧录提示

生成的 `.img` 使用 AiBurn 烧录。按住开发板 BOOT 键连接 USB，选择完整镜像后
烧录；UART0 使用 115200 8N1，关闭软硬件流控。详细启动和实机检查命令见项目
主 [README](README.md#43-烧录与启动)。

家庭屏固件的编译不依赖本地启动 `mijia-api`。只有实际使用米家 App 扫码登录时，
才需要在局域网服务器上部署对应服务。
