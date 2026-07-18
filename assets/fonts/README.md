# MiSans 字体资产

`MiSans-Regular.ttf` 是完整源字体，不是从界面字符串生成的字形子集。
`MiSans-Regular-18-full.bin` 包含源字体实际提供的全部 Unicode 字符，由
`lv_font_conv` 以 18 px、1 bpp、RLE 压缩和无 kerning 模式预编译。打包脚本将
二进制字体写入 SPI NOR 的 `font` 分区；板端通过 LVGL BinFont 一次加载，运行期
不再解析 TTF，也不执行 TinyTTF/STB 浮点光栅化。

- 源 TTF：8,122,324 字节，SHA-256
  `9c120f0a849bc0aa5048daae2a3c0f6eecd828b5b33fce682a9622833f5feea6`
- 运行时 BinFont：1,141,148 字节，SHA-256
  `b1aa4b9c025ea5268cb0049342bfc2f13dd0189545c9dabc81dfd52dcebeab47`
- 来源：[MiSans 官方页面](https://hyperos.mi.com/font/zh/)
- 许可：[MiSans 字体知识产权许可协议](https://hyperos.mi.com/font-download/MiSans%E5%AD%97%E4%BD%93%E7%9F%A5%E8%AF%86%E4%BA%A7%E6%9D%83%E8%AE%B8%E5%8F%AF%E5%8D%8F%E8%AE%AE.pdf)

MiSans 不是本项目的 Apache-2.0 开源代码，使用者应同时遵守其字体许可协议。
