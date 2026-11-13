# 版本

[English](VERSIONS.md)（以英文版为准）

每个发布都会写明其中所含各格式和组件的版本。有自身版本号的组件按版本号标识，没有的按其构建所用的本仓库提交哈希标识。

## 格式

| 格式 | 版本 | 规范 | 状态 |
|---|---|---|---|
| LAE Lossy（LAEY） | 容器版本 5 | [spec/lossy/SPEC.zh.md](../spec/lossy/SPEC.zh.md) | 正式 |
| LAE Lossless（LAES） | 有效载荷版本 2（版本 1 的文件仍然有效） | [spec/lossless/SPEC.zh.md](../spec/lossless/SPEC.zh.md) | 正式 |
| LAE CD-Extra 封装（CDX） | 版本 0 | [spec/cdx/SPEC.zh.md](../spec/cdx/SPEC.zh.md) | 草案 |
| LAENOISE 标记文件 | 版本 1 | [plugins/lae_noise_marker/SPEC_LAENOISE.zh.md](../plugins/lae_noise_marker/SPEC_LAENOISE.zh.md) | 正式 |

FLAC 应用 ID `LAE1`（LAEY）和 `LAE2`（LAES）已在 IANA 的 FLAC Application Metadata Block IDs 注册表（RFC 9639，第 12.2 节）中注册。

## 组件

| 组件 | 版本 | 读 / 写 |
|---|---|---|
| `lae_enc`、`lae_dec`（LAEY 编码器和解码器） | 发布时的提交哈希 | LAEY 容器 v5，WAV 和 FLAC 形式，扩展包 |
| `laes`（LAES 工具） | 发布时的提交哈希 | LAES 有效载荷 v1 和 v2 |
| `lae_cdx`（CD-Extra 工具） | 发布时的提交哈希 | CDX v0 |
| `lae_udec`（统一解码器）和 `liblae` | 发布时的提交哈希；`liblae` API 9 | LAEY v5（两种形式、扩展包）、LAES v1 和 v2 |
| `lae_noise` 和噪声面板 `noise_panel.html`（LAE Noise Marker） | 发布时的提交哈希 | LAENOISE v1 |
| foobar2000 组件 `foo_input_lae` | 0.30 | LAEY v5、LAES v1 和 v2 |
| Kodi 插件 `audiodecoder.lae` | 0.2.12 | LAEY v5、LAES v1 和 v2 |
| DirectShow 过滤器 `dshow_lae` | 0.2.11 | LAEY v5、LAES v1 和 v2 |
| VLC 模块 `vlc_lae` | 0.1.11 | LAEY v5、LAES v1 和 v2 |
| Java 库（`java/`） | 发布时的提交哈希 | LAEY v5、LAES v1 和 v2 |

## 兼容性

- 能读取某一格式版本的组件，旨在能够读取该版本的所有文件，但以 [spec/COMPATIBILITY.zh.md](../spec/COMPATIBILITY.zh.md) 及该组件 README 中所列的组件和平台已知限制为限。组件的后续发布不会改变已有文件的解码输出。
- 编码器是确定性的：相同的输入、选项和编码器提交哈希，在所有受支持的平台（[BUILD.zh.md](BUILD.zh.md) 第 3 节所列的构建）上得到逐字节相同的文件。因此，编码器版本以提交哈希标识。更换编码器提交哈希可能改变编码出的字节，但绝不会改变格式版本。
- Java 库包含 LAEY 和 LAES 的编码器和解码器。它与同一提交哈希的 C++ 工具（`lae_enc`、`lae_dec`、`laes`）编码出相同的文件、解码出相同的采样。
- 现有解码器无法读取的格式改动，会提高格式版本号。对于 CDX，该规则自其第一个正式版本起适用；版本 0 为草案，仍可能变化。

## 发布对照表

每个发布在本表中增加一行。

| 发布 | 仓库提交哈希 | LAEY | LAES | CDX | LAENOISE | foobar2000 | Kodi | DirectShow | VLC |
|---|---|---|---|---|---|---|---|---|---|
| v5-2026-11-07 | 标签 `v5-2026-11-07` | v5 | v1 | v0 | v1 | 0.20 | 0.2.5 | 0.2.4 | 0.1.4 |
| {NEXT_VERSION} | 标签 `{NEXT_VERSION}` | v5 | v2 | v0 | v1 | 0.30 | 0.2.12 | 0.2.11 | 0.1.11 |
