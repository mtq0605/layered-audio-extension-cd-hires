# 第三方组件与许可

[English](THIRD_PARTY.md)（以英文版为准）

本仓库中的全部源代码均为 LAE 项目编写，未复制任何第三方源代码。项目自身各文件的许可见 [REUSE.toml](REUSE.toml) 和 [LICENSE](LICENSE)；编码器各文件的来源记录在 [AUTHORS](AUTHORS) 中。

部分组件编译时依赖第三方 SDK 或头文件。这些依赖**不**包含在本仓库中，构建脚本以参数形式接收其位置。分发这类组件的编译产物时，须遵守下列说明。

## 命令行工具、参考解码器和编码器

| 依赖 | 许可 | 用途 |
|---|---|---|
| 编译器（MSVC、GCC、Clang）的 C++17 标准库和 C 运行库 | 编译器厂商的运行库条款 | 链接使用；Windows 版本静态链接 Microsoft C 运行库（`/MT`） |
| Windows 系统库（`kernel32`、`user32`、`shell32`、`ole32`、`advapi32`） | 随 Windows 提供 | 仅在 Windows 上链接 |
| Windows 光盘镜像服务 IMAPI2 | 随 Windows 提供 | `tools/laes_disc.ps1` 通过 COM 调用，不随附分发 |

以下为独立实现，未使用所提及项目的代码：

- 区间编码器（`src/part_stream.h`）为项目自行实现，其比特流与 LZMA 风格的编码器相同；
- FFT 和 DCT-IV（`src/fft.h`、`src/mdct_tb.h`）为项目自有代码，旋转因子的形式沿用公开文献；
- Windows 上的 UTF-8 命令行处理（`src/win_utf8.h`）沿用 FLAC `win_utf8_io` 的思路，未使用 FLAC 的代码。

## 播放器插件

| 插件 | 编译依赖 | 依赖的许可 | 对分发的编译产物的影响 |
|---|---|---|---|
| foobar2000（`foo_input_lae`） | foobar2000 SDK 2019-12-27（x86）和 2026-10-01（x64） | foobar2000 SDK 许可（BSD 风格） | SDK 的部分代码编译进组件；随编译产物附上 SDK 许可原文 |
| VLC（`vlc_lae`） | VLC 3.0 SDK、`libvlccore` | LGPL-2.1-or-later | 动态链接；附上 LGPL 说明，并注明 VLC 源代码的获取位置 |
| Kodi（`kodi_audiodecoder_lae`） | `kodi-dev-kit` 头文件，Kodi 21.3 "Omega" | GPL-2.0-or-later | 插件编译产物为结合作品，按 GPL-3.0-or-later 分发（Apache-2.0 与 GPL 第 3 版兼容，与第 2 版不兼容）；插件自身源代码仍为 Apache-2.0 |
| DirectShow（`dshow_lae`） | Windows SDK（`strmiids`、COM 库） | 随 Windows SDK 提供 | 无需额外说明 |
| LAE 噪声标记（`lae_noise_marker`） | 仅浏览器（HTML 和 JavaScript） | — | 无第三方代码 |

## Java 库

Java 库（`java/`）只使用 Java 标准库（Java 8 API 级别，Android API 24），没有第三方依赖。

## 本仓库以外的应用

嵌入 LAE 库的应用（例如手机播放器）是独立作品，有各自的许可和说明，其编译产物不从本仓库分发。

## 发布中的音频文件

发布附件中的演示音频和母带是作者本人的作品，适用每个发布所附的 `AUDIO_LICENSE`，不适用本仓库的许可。
