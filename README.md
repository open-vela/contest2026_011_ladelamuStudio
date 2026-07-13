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
| GT911 触摸 | 已验证 | `/dev/input0` 可报告按下、移动和抬起事件 |
| 系统定时器 | 已验证 | CORET 4 MHz 计数器轮询，`sleep`/`usleep` 正常 |

真机显示验证所使用镜像的 SHA-256 为：

```text
64e102b1d68e4b8148ac30f4c57941149c851348b1f32fe2db97b25fbf871855
```

真机触摸验证所使用镜像的 SHA-256 为：

```text
4dc5dcc6c6f9fffca4282d574286a0ccd89c625de6f123a54cbd1f01b549b5be
```

系统定时器轮询真机验证镜像的 SHA-256 为：

```text
3a1a6613bb8df7b0434c5d491f593781bdad1e4d33dacda33a2a7c90d94117d2
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
5. 确认 `/dev/i2c2`、`/dev/fb0`、`/dev/input0`、背光和 1024x600
   彩条均正常。
6. 按下、移动和松开触摸屏时分别执行
   `hexdump /dev/input0 count=32`，确认标志依次包含 `0x19`、`0x1a`
   和 `0x1c`。

## 关键实现说明

- `d13x_head.S` 使用 `jal x1, __start_c` 进入 C 代码；跳转目标和 C 函数
  序言保持真机验证版本不变。
- NuttX 从 `0x30040000` 的 PSRAM 执行，镜像入口与链接脚本地址一致。
- CORET IRQ 7 保持屏蔽，避免进入尚不稳定的 E907 CLIC 返回路径。idle
  任务按 4 MHz CORET 计数器每 10 ms 推进 NuttX 系统时钟，并同时保留
  UART0 与 GT911 轮询；真机已验证 `sleep`、`usleep` 和既有外设无回归。
- GT911 使用 I2C2 和 `/dev/input0`。由于 GPIO/CLIC 中断返回路径尚未纳入
  稳定基线，PA11 由 idle 任务轮询，I2C 读取仍在调用者任务上下文执行。
- 启动时保留面板内置 GT911 配置，避免通用配置表覆盖分辨率、坐标方向和
  传感器调校参数。

## 许可证

Apache-2.0
