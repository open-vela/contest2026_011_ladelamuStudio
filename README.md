# D13x openvela 硬件移植项目

> 2026 首届 openvela AI 硬件开发者大赛 — 新硬件平台适配赛道

## 项目简介

将 openvela（基于 NuttX 的 AIoT 操作系统）移植到匠芯创 D13x 系列 RISC-V MCU（衡山派 D133EBS 开发板），完成 BSP 适配与基础驱动开发，使系统能在目标硬件上启动并运行 NSH Shell。

**芯片亮点**：国产 RISC-V 玄铁 E907 内核 @ 480MHz，1MB SRAM + 8MB PSRAM，集成显示引擎（LVDS/RGB/MIPI）、2D 图形加速、JPEG/PNG 硬解、10/100M 以太网、USB 2.0 HS、8路 UART、4路 SPI、3路 I2C、2路 CAN。

## 目录结构

```
contest2026_011_ladelamuStudio/
├── README.md                           # 本文件
├── board/
│   └── d13x-hengshan-pi/              # D13x 板级代码
│       ├── configs/nsh/defconfig      # 最小 NSH 启动配置
│       ├── include/board.h            # 板级宏定义
│       ├── scripts/ld.script          # 链接脚本
│       ├── scripts/Make.defs          # 编译配置
│       ├── src/artinchip_boot.c       # 板级启动初始化
│       └── src/artinchip_appinit.c    # 应用初始化
├── bootloader-src/                    # D13x bootloader 源码
│   ├── src/startup.S                  # 启动汇编
│   ├── src/system.c                   # 系统初始化（时钟、UART）
│   ├── src/main.c                     # 主程序
│   ├── include/d13x_boot.h           # 头文件
│   ├── scripts/linker.ld             # 链接脚本
│   ├── tools/mk_aic.py               # .aic 镜像生成工具
│   └── Makefile                       # 编译脚本
├── app/                               # 示例应用
├── quickapp/                          # 快应用示例
├── logs/                              # AI Coding 日志
└── docs/                              # 参考文档
```

## 编译指南

### 环境要求

- RISC-V 工具链: xPack riscv-none-elf-gcc 14.2.0
- Python 3.x + pyyaml
- make, cmake

### 编译固件

```bash
# 进入 openvela 工作区
cd openvela

# 配置
./tools/configure.sh ../contest2026_011_ladelamuStudio/board/d13x-hengshan-pi/configs/nsh

# 编译
make -j$(nproc)

# 生成 bin 文件
riscv-none-elf-objcopy -O binary nuttx nuttx.bin

# 打包 .img 文件
cd vendor/artinchip/pack
bash pack.sh hengshan-pi d13x
```

### 编译 bootloader

```bash
cd bootloader-src
make
```

### 烧录

1. 按住 BOOT 按钮 + 插入 Type-C → 进入烧录模式
2. 使用 AiBurn 工具加载 `.img` 文件
3. 点击"开始"烧录

## 关键技术参数

| 参数 | 值 |
|------|-----|
| CPU | RISC-V 玄铁 E907, 480MHz |
| SRAM | 1MB @ 0x20000000 |
| PSRAM | 8MB @ 0x30000000 |
| Flash | NOR 16MB |
| UART0 | 0x18710000, 115200 8N1 |
| CMU | 0x18020000 |
| GPIO | 0x18700000 |

## 评分维度

| 维度 | 权重 | 策略 |
|------|------|------|
| 技术难度 | 30% | RISC-V + 国产芯片 + 外设驱动完整性 |
| 产品创新 | 20% | 国产芯片首次适配 openvela |
| 项目完整度 | 20% | 系统启动 + nsh 可用 + 文档完整 |
| AI 开发 | 10% | 使用 .claude Skills、记录 Token 消耗 |
| 商业潜力 | 10% | 智能家居中控屏场景 |
| 展示效果 | 10% | 触摸屏交互 Demo |

## 联系方式

- 队伍: contest2026_011_ladelamuStudio
- GitHub: https://github.com/lladlam/contest2026_011_ladelamuStudio
- 大赛仓库: https://github.com/lladlam/2026OpenVela

## License

Apache-2.0
