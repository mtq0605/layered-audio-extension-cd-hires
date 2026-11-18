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
| foobar2000 组件 `foo_input_lae` | 0.31 | LAEY v5、LAES v1 和 v2 |
| Kodi 插件 `audiodecoder.lae` | 0.2.13 | LAEY v5、LAES v1 和 v2 |
| DirectShow 过滤器 `dshow_lae` | 0.2.12 | LAEY v5、LAES v1 和 v2 |
| VLC 模块 `vlc_lae` | 0.1.12 | LAEY v5、LAES v1 和 v2 |
| Java 库（`java/`） | 发布时的提交哈希 | LAEY v5、LAES v1 和 v2 |

## 兼容性

- 能读取某一格式版本的组件，旨在能够读取该版本的所有文件，但以 [spec/COMPATIBILITY.zh.md](../spec/COMPATIBILITY.zh.md) 及该组件 README 中所列的组件和平台已知限制为限。组件的后续发布不会改变已有文件的解码输出。
- 编码器是确定性的：相同的输入、选项和编码器提交哈希，在所有受支持的平台（[BUILD.zh.md](BUILD.zh.md) 第 3 节所列的构建）上得到逐字节相同的文件。因此，编码器版本以提交哈希标识。更换编码器提交哈希可能改变编码出的字节，但绝不会改变格式版本。
- Java 库包含 LAEY 和 LAES 的编码器和解码器。它与同一提交哈希的 C++ 工具（`lae_enc`、`lae_dec`、`laes`）编码出相同的文件、解码出相同的采样。有一处限制是它自己的：LAES 格式和 C++ 工具允许一个文件有 255 个母带，Java 的播放器前端只为母带不超过 16 个的文件提供母带，Java 的 LAES 编码器拒绝超过 16 个母带。
- Java 解码器类 `com.lae.laey.LaeyDecoder` 有自己的接口编号 `LAE_API_VERSION` = 5。只有这个类加了函数时它才增加，不跟随 `liblae` 的 API 编号（9）。这个类有 C API 第 1 到 3 版的 LAEY 函数（`lae_open_reader`、`lae_stream_bytes`）和第 8 版的扩展包函数（`lae_pack_count`、`lae_pack_info`、`lae_select_pack`；没有 `lae_selected_pack`），没有 `lae_open_pair` 和 `lae_open_pair_files`；LAES 的函数在 `com.lae.laes` 中。
- 现有解码器无法读取的格式改动，会提高格式版本号。对于 CDX，该规则自其第一个正式版本起适用；版本 0 为草案，仍可能变化。
- 带扩展包的 LAEY 文件仍是容器版本 5。v5-2026-11-14 之前的解码器播放这种文件的 CD 层加主体（包 0），忽略其余的包：既不列出，也不提示文件里还有别的包。
- 解码器为 LAES 文件提供的派生 16 位版本在 2026-11-11 改了规则（[spec/lossless/SPEC.zh.md](../spec/lossless/SPEC.zh.md) 第 10.1 节）：v5-2026-11-07 的工具和插件每个采样率最多提供一个，v5-2026-11-14 的每个来源提供一个（来源是核心母带、每个母带及其每个采样率档位；发行方在编码时关掉的除外）。因此，对同一个文件，两个发布列出的条目个数、顺序和名称可能不同，按编号或名称选条目的脚本可能选到不同的条目。母带、母带的各个采样率档位和核心母带在两个发布中解码结果相同，母带 1 的派生版本逐位相同。
- v5-2026-10-18 的编码器读 32 位整数母带时只保留高 24 位，丢掉低 8 位，并且不提示。自 v5-2026-10-24 起，编码器先数出低 8 位不为零的采样；有这样的采样而又没有给出 `bits=24` 时，在控制台中询问，由脚本调用时则停止。低 8 位有内容的 32 位母带用 v5-2026-10-18 编码，得到的文件是该母带截成 24 位之后的结果。

## 发布对照表

每个发布在本表中增加一行。

| 发布 | 仓库提交哈希 | LAEY | LAES | CDX | LAENOISE | foobar2000 | Kodi | DirectShow | VLC |
|---|---|---|---|---|---|---|---|---|---|
| v5-2026-11-07 | 标签 `v5-2026-11-07` | v5 | v1 | v0 | v1 | 0.20 | 0.2.5 | 0.2.4 | 0.1.4 |
| v5-2026-11-14 | 标签 `v5-2026-11-14` | v5 | v2 | v0 | v1 | 0.30 | 0.2.12 | 0.2.11 | 0.1.11 |
| v5-2026-11-19 | 标签 `v5-2026-11-19` | v5 | v2 | v0 | v1 | 0.31 | 0.2.13 | 0.2.12 | 0.1.12 |
