# openvela D13x 衡山派移植

本仓库保存将 openvela 移植到匠芯创 D13x 衡山派开发板所需的参赛者代码。
目标芯片为 D133EBS，CPU 为玄铁 E907。

## 硬件验证基线

当前源码来自 2026-07-13 在真实开发板验证过的 `step20b` 冻结版本。

| 功能 | 状态 | 验证结果 |
| --- | --- | --- |
| SPI NOR 启动 | 已验证 | PBP 和 tinySPL 能加载并进入 NuttX |
| UART0 控制台 | 已验证 | 115200 8N1，NSH 可正常输入和输出 |
| NuttX VFS | 已验证 | 反复执行 `ls /dev` 不再触发异常 |
| I2C2 | 已验证注册 | `/dev/i2c2` 节点存在 |
| LVDS 显示 | 已验证 | `/dev/fb0`、1024x600 彩条、PE13 背光正常 |
| GT911 触摸 | 仅保留源码，默认关闭 | 等待后续真机运行验证 |
| 系统定时器 | IRQ 保持屏蔽 | 未纳入本次显示成功基线 |

真机显示验证所使用镜像的 SHA-256 为：

```text
64e102b1d68e4b8148ac30f4c57941149c851348b1f32fe2db97b25fbf871855
```

## 仓库结构

```text
board/d13x-hengshan-pi/   板级代码、NSH 配置和镜像打包输入
chip/d13x/                D13x 启动、中断、串口、I2C 和显示驱动
nuttx-overlay/            必须覆盖到上游 NuttX 的文件
vendor-overlay/           D13x 匠芯创打包工具输入和打包脚本
scripts/integrate.sh      将参赛代码安装到 openvela 工作区
scripts/build.sh          集成、配置、编译并打包镜像
logs/                     官方格式的 AI Coding 日志
```

目标文件、临时 ROMFS 头文件和历史测试镜像均为可再生构建产物，不作为移植
源码提交。

## 编译与打包

工作区中需要存在同级的 `nuttx`、`apps` 和 `vendor/artinchip`，并使用官方
`dev-ai-contest-2026` 分支。比赛 manifest 会链接新的板级和芯片目录；集成
脚本负责安装本仓库保存的 NuttX 与 vendor 覆盖文件。

主机需要 Python 3、CMake、Ninja、Kconfig 工具，以及命令前缀为
`riscv32-unknown-elf-*` 或 `riscv-none-elf-*` 的 RV32 GNU 裸机工具链。

```bash
cd contest2026_011_ladelamuStudio
./scripts/build.sh /path/to/openvela-workspace
```

最终可烧录镜像位于：

```text
vendor/artinchip/pack/prebuilt/d13x_hengshan-pi_v1.0.0.img
```

16 MiB SPI NOR 的分区合计使用 15 MiB：

| 分区 | 大小 |
| --- | ---: |
| spl | 512 KiB |
| env + env_r | 256 KiB |
| userid | 256 KiB |
| os | 3 MiB |
| rodata | 10 MiB |
| data | 1 MiB |

## 烧录与冒烟测试

1. 按住 BOOT 键连接开发板，使用 AiBurn 烧录生成的 `.img`。
2. 将 UART0 的 TX、RX、GND 连接到 3.3 V USB 转 TTL 模块。
3. 串口设置为 115200 8N1，并关闭硬件、软件流控。
4. 执行 `echo RX_OK`，再连续多次执行 `ls /dev`。
5. 确认 `/dev/i2c2`、`/dev/fb0`、背光和 1024x600 彩条均正常。

## 关键实现说明

- `d13x_head.S` 使用 `jal x1, __start_c` 进入 C 代码；跳转目标和 C 函数
  序言保持真机验证版本不变。
- NuttX 从 `0x30040000` 的 PSRAM 执行，镜像入口与链接脚本地址一致。
- CORET/GTC 路径保持关闭，因为 IRQ 7 的定时器实验未纳入显示成功基线。
- GT911 代码用于下一次独立功能提交；当前 `configs/nsh/defconfig` 不启用
  触摸配置。

## 许可证

Apache-2.0
