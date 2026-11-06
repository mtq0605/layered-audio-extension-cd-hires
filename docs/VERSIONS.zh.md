# 版本

[English](VERSIONS.md)（以英文版为准）

每个发布都会写明其中所含各格式和组件的版本。有自身版本号的组件按版本号标识，没有的按其构建所用的本仓库提交号标识。

## 格式

| 格式 | 版本 | 规范 | 状态 |
|---|---|---|---|
| LAE Lossy（LAEY） | 容器版本 5 | [spec/lossy/SPEC.zh.md](../spec/lossy/SPEC.zh.md) | 稳定 |
| LAE Lossless（LAES） | 载荷版本 1 | [spec/lossless/SPEC.zh.md](../spec/lossless/SPEC.zh.md) | 稳定 |
| LAE CD-Extra 封装（CDX） | 版本 0 | [spec/cdx/SPEC.md](../spec/cdx/SPEC.md) | 草案 |
| LAENOISE 标记文件 | 版本 1 | [plugins/lae_noise_marker/SPEC_LAENOISE.zh.md](../plugins/lae_noise_marker/SPEC_LAENOISE.zh.md) | 稳定 |

FLAC 应用 ID `LAE1`（LAEY）和 `LAE2`（LAES）已在 IANA 注册。

## 组件

| 组件 | 版本 | 读 / 写 |
|---|---|---|
| `lae_enc`、`lae_dec`（LAEY 编码器和解码器） | 发布时的提交号 | LAEY 容器 v5 |
| `laes`（LAES 工具） | 发布时的提交号 | LAES 载荷 v1 |
| `lae_cdx`（CD-Extra 工具） | 发布时的提交号 | CDX v0 |
| `lae_noise` 和噪声面板 | 发布时的提交号 | LAENOISE 1 |
| foobar2000 组件 `foo_input_lae` | 0.19 | LAEY v5、LAES v1 |
| Kodi 插件 `audiodecoder.lae` | 0.2.4 | LAEY v5、LAES v1 |
| DirectShow 过滤器 `dshow_lae` | 0.2.3 | LAEY v5、LAES v1 |
| VLC 模块 `vlc_lae` | 0.1.3 | LAEY v5、LAES v1 |
| Java 库（`java/`） | 发布时的提交号 | LAEY v5、LAES v1 |

## 兼容性

- 能读取某一格式版本的组件，能读取该版本的所有文件。组件的新版本不会改变已有文件的解码输出。
- 编码器是确定性的：相同的输入、选项和编码器提交号，在所有受支持的平台上得到逐字节相同的文件，因此编码器版本以提交号命名。更换编码器提交号可能改变编码出的字节，但不会改变格式版本。
- Java 库与同一提交号的 C++ 工具编码出相同的文件、解码出相同的采样。
- 现有解码器无法读取的格式改动，会提高格式版本号。

## 发布对照表

每个发布增加一行。

| 发布 | 仓库提交号 | LAEY | LAES | CDX | LAENOISE | foobar2000 | Kodi | DirectShow | VLC |
|---|---|---|---|---|---|---|---|---|---|
| *下一个* | *发布时填写* | v5 | v1 | v0 | 1 | 0.19 | 0.2.4 | 0.2.3 | 0.1.3 |
