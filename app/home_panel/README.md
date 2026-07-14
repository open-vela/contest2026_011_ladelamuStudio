# D13x 家庭中控屏

这是运行在 D13x 衡山派上的 1024x600 横屏 LVGL 原生应用。应用源码通过比赛
manifest 映射到 `packages/demos/contest2026_011_home_panel`；本仓库的
`scripts/integrate.sh` 同时提供不使用 `repo` 工作区时的本地构建映射。

当前应用编译为 NSH builtin：

```text
nsh> home_panel
```

界面直接使用 `/dev/fb0` 的 RGB565 双缓冲和 `/dev/input0` 的 GT911 触摸事件。
账号授权与设备数据后续通过有线网络连接服务器，米家账号凭据不保存在开发板。

SPI NOR 使用 15 MiB 的显式分区布局：系统 3 MiB、只读资源 4 MiB、持久化
数据 7 MiB，其余 1 MiB 留作介质余量。只读资源分区可容纳当前约 2.3 MiB
资源，同时为登录会话、设备缓存和家庭配置保留足够可写空间。

## 开源项目与第三方组件

### 固件运行时依赖

- [OpenVela](https://github.com/open-vela/openvela)：系统工作区、板级集成与应用框架。
- [Apache NuttX](https://github.com/apache/nuttx)：RTOS 内核、NSH、framebuffer、
  I2C 和 touchscreen 子系统，Apache-2.0 许可证。
- [LVGL](https://github.com/lvgl/lvgl)：家庭中控屏图形与事件框架，MIT 许可证；
  当前固件使用 LVGL 9.2.1。
- [ArtInChip Luban-Lite](https://gitee.com/artinchip/luban-lite)：D13x 启动、显示、
  时钟与外设实现的移植参考；其代码不作为本应用的独立运行时库加载。

### 服务端接口参考

- [mijia-api](https://github.com/Do1e/mijia-api)：米家 App 扫码登录、家庭、设备、
  属性和场景接口参考，GPL-3.0 许可证。该项目运行在服务器侧，不链接进 D13x
  固件，开发板只通过后续定义的有线网络 API 与服务器通信。

### 字体与生成工具

- [MiSans](https://hyperos.mi.com/font/zh/)：界面中文字体。固件只包含当前界面
  所需字符生成的 18px 位图字形，并依据
  [MiSans 字体知识产权许可协议](https://hyperos.mi.com/font-download/MiSans%E5%AD%97%E4%BD%93%E7%9F%A5%E8%AF%86%E4%BA%A7%E6%9D%83%E8%AE%B8%E5%8F%AF%E5%8D%8F%E8%AE%AE.pdf)
  使用和注明；MiSans 本身不是本项目的开源代码。
- [lv_font_conv](https://github.com/lvgl/lv_font_conv)：将本地 MiSans 字体裁剪并
  转换为 LVGL C 字库的构建工具，MIT 许可证；正常固件编译不依赖 Node.js。
