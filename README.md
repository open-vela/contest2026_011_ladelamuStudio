# OpenVela D13x 衡山派家庭中控屏

当前发布版本：**1.10.11.7**。版本变化见 [CHANGELOG.md](CHANGELOG.md)。

## 一、作品简介

本作品将 OpenVela（NuttX-based RTOS）移植到匠芯创 D13x 衡山派开发板，
目标芯片为 D133EBS，CPU 为玄铁 E907。在完整启动链路的基础上实现了 UART0
交互控制台、LVDS 1024x600 显示、GT911 电容触摸、RMII 百兆以太网和横屏家庭
中控应用。

家庭屏开机自动运行，可显示家庭、房间、场景和设置界面，并通过有线网络区分
“有线网络未连接”“无互联网连接”和“已连接互联网”状态。应用通过局域网内的
独立 `mijia-api` 服务完成米家 App 扫码登录，并同步家庭名称、设备总数与在线数；
小米账号 Cookie 不存放在开发板，板端只持久保存自动恢复所需的服务令牌和保险箱
解锁口令。

作品亮点：

- 从 PBP/tinySPL 到 NuttX、NSH 和图形应用的完整 D13x 启动链路。
- 显示、触摸、系统定时器和以太网均经过开发板实机联合验证。
- 适配 16 MiB SPI NOR，分区容量完整覆盖系统、字体和持久化数据。
- 完整 MiSans 字符集预编译为 1 bpp LVGL BinFont 并存放在独立 Flash
  分区，动态米家设备名和房间名不再受静态字形子集限制。
- 米家状态采用带连续版本的 MIoT 增量补丁；常规属性变化不再重复传输和解析
  完整家庭快照，版本断档或家庭结构变化时自动回退全量同步。

## 二、选题方向

**新硬件适配**。

作品的核心是为 OpenVela 增加 D13x/D133EBS 芯片与衡山派板级支持，覆盖启动、
中断、串口、I2C、显示、触摸、定时器、网络和镜像打包。家庭中控屏作为完整应用
验证这些驱动能够同时稳定运行，也体现了 AI 硬件产品创新方向的实际落地能力。

## 三、目录结构

```text
app/home_panel/          1024x600 家庭中控 LVGL 应用、网络检测与字体流式加载
assets/fonts/            MiSans 源 TTF、完整字符集 BinFont 及资源说明
board/d13x-hengshan-pi/  板级初始化、NSH 配置、链接脚本和镜像打包输入
chip/d13x/               D13x 启动、中断、串口、I2C、显示、定时器和 GMAC 驱动
nuttx-overlay/           需要覆盖到上游 NuttX 的架构、驱动和调度文件
vendor-overlay/          匠芯创镜像打包工具所需的补充文件
patches/                 上游组件兼容性补丁
scripts/integrate.sh     将本仓源码集成到 OpenVela 工作区
scripts/build.sh         配置、编译并打包可烧录镜像
logs/                    官方格式的 AI Coding 对话日志
.github/workflows/       GitHub Actions 构建与 Release 流程
VERSION                  四段累计版本号
CHANGELOG.md             版本更新记录
```

`app/hello_app/`、`board/contest_board/` 和 `quickapp/hello_quickapp/` 为比赛仓初始
示例；D13x 成品实现集中在上表列出的目录。目标文件、临时 ROMFS 头文件和历史
测试镜像均为可再生构建产物，不提交到源码仓库。

## 四、运行方式

第一次在新电脑上搭建环境，请直接阅读
[D13x 衡山派新电脑编译指南](BUILDING.md)。其中包含 Ubuntu/Debian、Arch Linux、
固定版本 RISC-V 工具链、完整工作区克隆、构建产物和常见错误处理步骤。

### 4.1 准备工作区

按照比赛官方流程准备 OpenVela `repo` 工作区。工作区中应存在以下同级目录，
并使用官方 `dev-ai-contest-2026` 分支：

```text
<workspace>/
├── nuttx/
├── apps/
├── vendor/artinchip/
└── contest2026_011_ladelamuStudio/
```

主机需要 Bash、Python 3、CMake、Ninja、Kconfig 工具、`curl`、`unzip`、
`genromfs`，以及命令前缀为 `riscv32-unknown-elf-*` 或
`riscv-none-elf-*` 的 RV32 GNU 裸机工具链。首次构建时集成脚本会获取固定版本
LVGL 9.2.1。

### 4.2 编译与打包

```bash
cd <workspace>/contest2026_011_ladelamuStudio
./scripts/build.sh <workspace>
```

脚本会依次集成参赛源码、生成 D13x NSH 配置、编译 NuttX 并调用 ArtInChip
打包工具。输出文件为：

```text
<workspace>/nuttx/nuttx
<workspace>/vendor/artinchip/pack/prebuilt/d13x_hengshan-pi_v<VERSION>.img
```

每次 PR 会在 GitHub Actions 中执行同样的构建。PR 合并到官方开发分支且构建
成功后，工作流按 `VERSION` 自动创建版本标签和 GitHub Release。

### 4.3 烧录与启动

1. 按住开发板 BOOT 键连接 USB，使用 AiBurn 烧录生成的 `.img`。
2. 将 UART0 的 TX、RX、GND 连接到 3.3 V USB 转 TTL 模块。
3. 串口设置为 115200 8N1，并关闭硬件和软件流控。
4. 复位后确认 tinySPL 加载 APP，随后出现 `NuttShell (NSH)`。
5. 家庭屏由板级 bring-up 自动启动；也可在 NSH 中执行 `home_panel` 手动启动。

### 4.4 实机验证

```text
nsh> echo RX_OK
nsh> ls /dev
nsh> hexdump /dev/input0 count=32
nsh> renew eth0
nsh> ifconfig
nsh> ping 192.168.1.1
```

预期 `/dev` 下包含 `i2c2`、`fb0`、`input0` 和 `ttyS0`；屏幕显示 1024x600
家庭界面，触摸产生按下、移动和抬起事件。有线网络连接后，DHCP 获取地址，家庭
屏状态最终更新为“已连接互联网”。

16 MiB SPI NOR 分区如下：

| 分区 | 大小 |
| --- | ---: |
| spl | 512 KiB |
| env + env_r | 256 KiB |
| userid | 256 KiB |
| os | 3 MiB |
| font | 8 MiB（完整字符集的预编译 MiSans BinFont） |
| data | 4 MiB |

米家登录凭据使用冗余记录保存在 `userid` 分区末尾。该分区不包含在升级镜像的
`target` 列表内，因此正常重新烧录系统、资源和数据分区不会清除登录状态。

## 五、AI Coding 使用说明

本作品使用 AI 辅助完成了以下开发环节：

- 需求拆解：将 D13x 移植拆分为启动、内存、中断、显示、触摸、网络和应用阶段。
- 方案设计：对照 D12x OpenVela、D13x 官方资料和 Luban-Lite，审查启动入口、
  PSRAM 地址、CORET/CLIC、显示时序与 GMAC 描述符约束。
- 编码与审查：生成或修改板级代码、驱动、配置、构建脚本和家庭屏应用，并通过
  差异审查控制修改范围。
- 实机调试：根据 UART 异常现场、寄存器转储、NSH 命令结果与 `tcpdump` 抓包
  逐步定位启动异常、缓存行长度、DHCP 帧、GT911 空样本和 LVGL 刷新问题。
- 文档与交付：维护四段版本号、更新日志、复现步骤、AI Coding 日志和自动发布
  工作流。

AI 显著缩短了跨芯片资料比对和故障假设验证时间；所有关键功能仍以编译结果、
GitHub Actions 和开发板实机输出作为最终验收依据。完整对话日志见 `logs/`。

## 六、开源项目与第三方组件

### 6.1 开源项目

| 项目 | 本作品中的用途 | 版本/范围 | 许可证与集成方式 |
| --- | --- | --- | --- |
| [OpenVela](https://github.com/open-vela/openvela) | 工作区、系统集成与应用框架 | `dev-ai-contest-2026` | Apache-2.0；本作品作为 OpenVela 参赛扩展提交 |
| [Apache NuttX](https://github.com/apache/nuttx) | RTOS 内核、NSH、VFS、网络、framebuffer、I2C 和触摸子系统 | OpenVela 对应分支 | Apache-2.0；固件运行时依赖 |
| [LVGL](https://github.com/lvgl/lvgl) | 家庭中控屏图形、布局与输入事件 | 9.2.1 | MIT；由 `scripts/integrate.sh` 固定版本集成 |
| [cJSON](https://github.com/DaveGamble/cJSON) | 解析米家登录、家庭与设备 API 响应 | 1.7.12 | MIT；通过 OpenVela `NETUTILS_CJSON` 集成 |
| [lv_font_conv](https://github.com/lvgl/lv_font_conv) | 生成字体分区不可用时的最小 LVGL 回退字库 | 仅生成阶段 | MIT；不作为固件运行时依赖 |
| [mijia-api](https://github.com/Do1e/mijia-api) | 米家 App 扫码登录、家庭、设备、属性和场景的服务端接口参考 | GPL-3.0-or-later | 仅运行在服务器侧，不复制或链接进 D13x 固件 |
| [Xiaomi Home Integration](https://github.com/XiaoMi/ha_xiaomi_home) | 核对小米官方 HTTP 控制、MQTT 状态订阅与 MIoT-Spec 消息架构 | 官方主分支 | Apache-2.0；仅作协议与架构参考，不复制进固件 |
| [ArtInChip Luban-Lite](https://gitee.com/artinchip/luban-lite) | D13x 启动、时钟、显示和外设寄存器参考 | 参考代码 | 不作为独立运行时库；使用时遵循其上游许可声明 |

服务端使用 [lladlam/mijia](https://github.com/lladlam/mijia) 维护的独立
`mijia-api` 工作副本，生产实例部署在 `https://mi.lladlam.top`。它与本仓库、
D13x 固件和 Release 产物相互独立，避免 GPL 服务端实现与 Apache-2.0 固件发生
代码链接。

### 6.2 字体资源

[MiSans](https://hyperos.mi.com/font/zh/) 用于家庭屏中文显示。MiSans 不是本项目
的开源代码；构建资产保留完整源字体 `MiSans-Regular.ttf`，镜像的独立
`font` 分区保存覆盖该字体全部字符的 `MiSans-Regular-18-full.bin`。板端通过
LVGL BinFont 读取，不在运行时解析 TTF 或执行浮点光栅化。BinFont SHA-256 为
`b1aa4b9c025ea5268cb0049342bfc2f13dd0189545c9dabc81dfd52dcebeab47`，并按照
[MiSans 字体知识产权许可协议](https://hyperos.mi.com/font-download/MiSans%E5%AD%97%E4%BD%93%E7%9F%A5%E8%AF%86%E4%BA%A7%E6%9D%83%E8%AE%B8%E5%8F%AF%E5%8D%8F%E8%AE%AE.pdf)
使用和注明。

## 七、实机验证状态

| 功能 | 状态 | 验证结果 |
| --- | --- | --- |
| SPI NOR 启动 | 已验证 | PBP 和 tinySPL 能加载并进入 NuttX |
| UART0 控制台 | 已验证 | 115200 8N1，NSH 可正常输入和输出 |
| NuttX VFS | 已验证 | 反复执行 `ls /dev` 不再触发异常 |
| LVDS 显示 | 已验证 | `/dev/fb0`、1024x600 RGB565 和 PE13 背光正常 |
| GT911 触摸 | 已验证 | `/dev/input0` 报告按下、移动和抬起事件 |
| 系统定时器 | 已验证 | CORET 4 MHz 架构定时器持续推进系统时钟 |
| GMAC0 以太网 | 已验证 | RMII 100M 全双工，DHCP、网关及外网连通正常 |
| 米家扫码登录 | 已验证 | 米家 App 扫码确认、令牌领取、家庭与设备统计正常 |
| 米家服务端 | 已验证 | `mi.lladlam.top` 使用有效 HTTPS 证书，健康检查和二维码接口正常 |

联合验证镜像 SHA-256：

```text
569f4948b428b21379f91db43beb9432406725abbb6be75a5ded77f4c3acf7cc
```

## 八、许可证

本作品原创代码采用 Apache-2.0 许可证。第三方项目和字体资源分别遵循第六节
列出的许可证或许可协议。
