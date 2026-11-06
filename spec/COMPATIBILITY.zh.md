# 不用 LAE 软件播放 LAE 文件

[English](COMPATIBILITY.md) | **中文**

LAES 是什么、优缺点：[lossless/OVERVIEW.zh.md](lossless/OVERVIEW.zh.md)。

LAE 文件就是普通的 WAV 或 FLAC 文件。不支持 LAE 的播放器播放 LAEY 文件的 **CD 层**、LAES 文件的 **核心母带**，其余部分跳过。LAES 的核心母带用 16 bit、44.1 / 48 kHz 时，可以是 CD 兼容的；Hi-Res 或 FLAC 形式的文件，CD 播放器读不了。本页列出测过什么、不支持 LAE 的播放器在哪些情况下会播放不了，以及原因。

**下面这些失败都不是 LAE 数据造成的**，而是那个解码器自身的限制：换成同类的普通 WAV 或 FLAC 文件，也会一样失败。要播放高解析度的层或无损母带，请用 LAE 的插件或播放器。

## 不支持 LAE 的播放器看到什么

| 文件 | 容器 | 不用 LAE 时播放的核心母带 | 跳过的部分 |
|---|---|---|---|
| LAEY（`.wav`） | RIFF WAVE | `data` 块：16 bit PCM，44.1 或 48 kHz（CD 层） | `"LAE "` 块（深度层和频率层） |
| LAES，WAV 形式（`.wav`） | RIFF WAVE；超过 4 GiB 时是 RF64 | `data` 块：核心母带（默认 16 bit） | `"LAE "` 块（母带） |
| LAES，FLAC 形式（`.flac`） | FLAC | 音频帧：核心母带（默认最多 24 bit） | 放在音频帧前面的 `LAE2` APPLICATION 块（母带） |

## 实测结果（2026-11-05）

测试文件：
- 演示文件里最大的几个：
  - `Grand_LAES.flac`：938 MB，第一个音频帧前面有 888 MB 元数据，16 bit 核心母带，10:05；
  - `Grand_LAES.wav`：995 MB；
  - 一个 241 MB 的 LAEY 文件，13:22。
- 10 秒的 LAES 片段：核心母带 24 bit 或 16 bit，分别做成 RIFF、RF64、FLAC。

凡标"能播"的，都和参照解码逐字节核对过。

| 解码器 | LAEY WAV | LAES WAV（RIFF） | LAES WAV（RF64） | LAES FLAC，16/24 bit 核心母带 | 普通 32 bit FLAC（对照） |
|---|---|---|---|---|---|
| libFLAC 1.3.3（官方 `flac` 命令行） | — | — | — | 能播 | — |
| FFmpeg（2025、2026 版） | 能播 | 能播 | 能播 | 能播 | — |
| Android 13 MediaExtractor / MediaCodec（索尼 Xperia） | 能播 | 能播 | **打不开** | 能播 | **不认** |
| Windows 11 Media Foundation（Windows 自带播放器、资源管理器） | — | — | — | **打不开**（见下） | **能打开，但不能解码** |

### 软件实测记录

下表是按普通用户的用法实测过的软件；除非注明，都没有装 LAE 软件。"能播"指：显示出时长，能开始播放，能跳转。表里没有的软件就是没测过。

| 软件（版本） | 日期 | 文件 | 结果 |
|---|---|---|---|
| Kodi 21.3（Windows x64；用它自己的解码器，即 FFmpeg） | 2026-11-05 | `Grand_LAES.flac`（LAES FLAC，938 MB） | 能播核心母带：10:04，44.1 kHz；能跳转 |
| Kodi 21.3 | 2026-11-05 | `Grand_LAES.wav`（LAES WAV，995 MB） | 能播核心母带：10:04，44.1 kHz；能跳转 |
| Kodi 21.3 | 2026-11-05 | `03_LAES_demo.lae.wav`（LAEY，241 MB） | 能播 CD 层：13:22，44.1 kHz；能跳转 |
| Kodi 21.3，装了 LAE 插件 0.2.2 | 2026-11-05 | LAES 的 `.lae` 片段 | 能播母带：352.8 kHz；能跳转 |
| foobar2000 2.26.2（64 位）和 1.5.1（32 位） | 2026-11-05 | `Grand_LAES.flac`、`Grand_LAES.wav`、`03_LAES_demo.lae.wav` | 三个都能打开，识别为 FLAC / PCM、44.1 kHz，也就是核心母带（LAES）或 CD 层（LAEY）。只把文件加进了播放列表，没有播放：测试时没有桌面控制权限 |
| foobar2000 2.26.2（64 位）和 1.5.1（32 位），装了 LAE 组件 0.16 | 2026-11-05 | `Grand_LAES.flac`、LAES FLAC 片段 | 识别为 LAES，读出了母带列表和要播放的母带（352.8 kHz）。没有播放（同上）；组件用的解码器就是其他播放器用的那一个 |
| VLC 3.0.24（64 位便携版） | 2026-11-05 | `Grand_LAES.flac` | 能播核心母带；用 VLC 解码成文件，与参照逐字节相同 |
| VLC 3.0.24 | 2026-11-05 | `Grand_LAES.wav`、`03_LAES_demo.lae.wav` | 能播核心母带（LAES）和 CD 层（LAEY）。用 VLC 解码成文件后与参照相同，只是最后 346 帧（8 毫秒）没有了。普通 WAV 在 VLC 里也少同样的帧，是 VLC 自己处理 WAV 的方式，与 LAE 数据无关 |
| VLC 3.0.24，装了 LAE 模块 0.1.1 | 2026-11-05 | LAES FLAC 片段 | 播放母带 `master_2_352800_32`；用 VLC 解码成文件，除最后 40 毫秒外与母带逐字节相同（VLC 处理流结尾的方式，同上） |
| Windows 11（版本 26200）的 Media Foundation：媒体播放器、"电影和电视"、资源管理器 | 2026-11-05 | `Grand_LAES.flac`、LAES FLAC 片段 | 不认（限制 1）。普通 24 bit FLAC 能播；普通 32 bit FLAC 能打开但不能解码 |
| Windows 11 资源管理器 | 2026-11-05 | `Grand_LAES.flac` | 时长、码率、标题都是空的（限制 1） |
| Android 13 系统解码器（索尼 Xperia XQ-BC72）：把文件交给系统去播的 App 都用它 | 2026-11-05 | 见上一张表 | 见上一张表 |
| DirectShow 播放器（PotPlayer、MPC-BE、MPC-HC），装了 LAE 滤镜 0.2.1 | 2026-11-05 | `Grand_LAES.flac`、`03_LAES_demo.lae.wav` | 母带 352.8 kHz，609 s，能跳转。LAEY 文件也能跳转，但跳转后约 4 秒才继续播放，0.2.0 也一样（直接调用滤镜测的，没有打开播放器窗口） |

用 libFLAC 或 FFmpeg 的播放器，表现应该和这两个库相同，例如 foobar2000、VLC、Kodi、mpv、PotPlayer、MPC-BE 和大多数 App。这些播放器只在库这一层测过，没有逐个在界面里试。

## 其他解码器的已知限制

1. **Windows 自带的 FLAC 解码器与 LAES FLAC**
   - Windows 的 Media Foundation 只在 FLAC 文件开头 4 MiB 以内找第一个音频帧；实测界限是音频从第 4,194,308 字节开始。媒体播放器、"电影和电视"都用它，资源管理器读时长和标签也用它。
   - 带母带的 LAES FLAC 把母带放在音频帧前面，这是 RFC 9639 对所有元数据的规定。所以第一个音频帧开始得更晚，Windows 就不认这个文件。
   - 这个解码器也不解码 32 bit FLAC。
   - 文件本身是合法的 FLAC，libFLAC 和 FFmpeg 都能播。
   - **替代办法：** 用 WAV 形式，或用基于 libFLAC / FFmpeg 的播放器，或装 LAE 插件。
2. **32 bit FLAC**
   - 很多解码器不播 32 bit FLAC：Android 13 的解码器不认，Windows 的解码器能打开但不能解码。
   - 所以 LAES 参考编码器默认核心母带最多用 24 bit，除非指定 `--core-bits=32`。要兼容尽量多的设备，就不要用 `--core-bits=32`。
3. **RF64（LAES 的 WAV 形式超过 4 GiB 时）**
   - RIFF WAV 不能超过 4 GiB，所以更大的 LAES WAV 文件（例如很长、母带很多的专辑）写成 RF64。
   - 很多 WAV 读取器不支持 RF64，Android 13 的解码器就打不开。
   - 这类文件请用 FLAC 形式。LAEY 文件总是普通 RIFF。
4. **`.lae` 扩展名**
   - 按文件名选解码器的播放器，没有 LAE 插件就打不开 `.lae`，例如 foobar2000、Windows 媒体播放器和很多手机 App。
   - 按内容识别的播放器会播放核心母带（LAEY 是 CD 层）。
   - 想在任何地方都能播放核心母带，就按文件的形式改名为 `.wav` 或 `.flac`。
5. **直接在网上串流文件本身**
   - FLAC 形式里，核心母带的音频帧在其他母带的数据后面。从头开始通过网络读取文件的播放器，要先把全部母带读完，才能出声。
   - 做流媒体请用 `laes_export` 导出普通 FLAC 来提供（核心母带、某个母带或某个采样率档，只选一种）。
6. **跳转（seek）**
   - LAES FLAC 按设计不写 SEEKTABLE，播放器靠搜索音频帧来跳转。能用，但在某些播放器里会慢一些。
   - WAV 跳转不受影响。
7. **WAV 里的标签**
   - LAES 把同一套标签写两遍：RIFF LIST/INFO 和 ID3v2.4（`id3 ` 块），都用 UTF-8。
   - 大多数播放器读 ID3。按系统代码页读 INFO 的老 Windows 程序，可能把非 ASCII 文字显示成乱码。
   - FLAC 的标签是 Vorbis comment，按 FLAC 规定用 UTF-8。

## 播放器播不了 LAE 文件时

- **文件名是 `.lae`：** 按文件的形式改名为 `.wav` 或 `.flac`，或者装 LAE 插件。
- **FLAC 文件，用的是 Windows 自带播放器：** 改用 WAV 形式、换一个播放器，或装 LAE 插件（限制 1）。
- **FLAC 文件，核心母带是 32 bit：** 把核心母带编成 24 bit（限制 2）。
- **WAV 文件超过 4 GiB：** 它是 RF64，请改用 FLAC 形式（限制 3）。
- **都不是：** 用 `laes_tool verify` 或 `laes_tool parts` 检查文件，再用 FFmpeg 试一下（`ffmpeg -i 文件 -f null -`）。FFmpeg 能播，问题就在那个播放器。
