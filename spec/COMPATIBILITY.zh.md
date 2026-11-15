# 不使用 LAE 软件播放 LAE 文件

[English](COMPATIBILITY.md) | **中文**（以英文版为准）

关于 LAES 的定义及其优点与局限，参见 [lossless/OVERVIEW.zh.md](lossless/OVERVIEW.zh.md)。

LAE 文件是普通的 WAV 或 FLAC 文件。不支持 LAE 的播放器播放 LAEY 文件的 **CD 层**和 LAES 文件的**核心母带**，并跳过其余部分。采用 16 bit、44.1 / 48 kHz 的 LAES 核心母带可以是 CD 兼容的；高解析度文件和 FLAC 形式的文件不能由 CD 播放器读取。本页列出已测试的内容、不支持 LAE 的播放器可能失败的情形及其原因。**上述失败均非 LAE 数据所致**：每一项都是相应解码器自身的限制，换用同类的普通 WAV 或 FLAC 文件时会以同样方式出现。如需播放高解析度层或无损母带，请使用 LAE 插件或 LAE 播放器。

## 不支持 LAE 的播放器所见内容

| 文件 | 容器 | 不使用 LAE 时播放的内容（核心母带；LAEY 为 CD 层） | 跳过的部分 |
|---|---|---|---|
| LAEY（`.wav`） | RIFF WAVE | `data` 块：16 bit PCM，44.1 或 48 kHz（CD 层） | `"LAE "` 块（位深层和频率层） |
| LAEY，FLAC 形式（`.flac`） | FLAC | 音频帧：16 bit，44.1 或 48 kHz（CD 层） | 位于音频帧之前的 `LAE1` APPLICATION 块（位深层、频率层和扩展包）。2026-11-10 用 GStreamer `flacdec` 核对：输出与 WAV 形式的 CD 层相同。Windows 自带的 FLAC 解码器：扩展数据超过约 4 MiB（几十秒音乐之后）时，适用限制 1 |
| LAES，WAV 形式（`.wav`） | RIFF WAVE，超过 4 GiB 时为 RF64 | `data` 块：核心母带（默认 16 bit） | `"LAE "` 块（母带） |
| LAES，FLAC 形式（`.flac`） | FLAC | 音频帧：核心母带（默认至多 24 bit） | 位于音频帧之前的 `LAE2` APPLICATION 块（母带） |

## 测试结果（2026-11-05）

测试文件为最大的几个演示文件以及若干短片段：
- `Grand_LAES.flac`：938 MB，其中第一个音频帧之前的元数据为 888 MB；16 bit 核心母带（独立 CD），26,667,616 帧 = 604.7 s（10:04.7；Kodi 显示为 10:04）；其母带时长为 609.3 s（10:09；下表中 DirectShow 一行播放的是母带）；
- `Grand_LAES.wav`：995 MB；
- 一个 LAEY 文件：241 MB；13:22；
- 10 秒的 LAES 片段，核心母带为 24 bit 或 16 bit，分别采用 RIFF、RF64 和 FLAC。

下表中凡标为"能播放"的结果，均已与参考解码结果逐字节比对。软件实测记录中的比对情况在该记录中各自说明。

| 解码器 | LAEY WAV | LAES WAV（RIFF） | LAES WAV（RF64） | LAES FLAC，16/24 bit 核心母带 | 普通 32 bit FLAC（对照） |
|---|---|---|---|---|---|
| libFLAC 1.3.3（参考 `flac` 命令） | — | — | — | 能播放 | — |
| FFmpeg（2025 年和 2026 年构建版本） | 能播放 | 能播放 | 能播放 | 能播放 | — |
| Android 13 MediaExtractor / MediaCodec（索尼 Xperia） | 能播放 | 能播放 | **无法打开** | 能播放 | **无法识别** |
| Windows 11 Media Foundation（Windows 自带播放器、资源管理器） | — | — | — | **无法打开**（见下文） | **能打开，但不能解码** |

### 软件实测记录

本表列出按普通用户使用方式测试的应用程序；除另有说明外，均未安装 LAE 软件。"能播放"指显示时长、开始播放且跳转正常。未列出的应用程序未经测试。

| 应用程序（版本） | 日期 | 文件 | 结果 |
|---|---|---|---|
| Kodi 21.3（Windows x64；内置解码器：FFmpeg） | 2026-11-05 | `Grand_LAES.flac`（LAES FLAC，938 MB） | 播放核心母带：10:04，44.1 kHz；跳转正常 |
| Kodi 21.3 | 2026-11-05 | `Grand_LAES.wav`（LAES WAV，995 MB） | 播放核心母带：10:04，44.1 kHz；跳转正常 |
| Kodi 21.3 | 2026-11-05 | `03_LAES_demo.lae.wav`（作品 *LAES demo* 的 LAEY 文件，241 MB） | 播放 CD 层：13:22，44.1 kHz；跳转正常 |
| Kodi 21.3，安装 LAE 插件 0.2.2 | 2026-11-05 | 一个 LAES `.lae` 片段 | 播放母带：352.8 kHz；跳转正常 |
| foobar2000 2.26.2（64 位）和 1.5.1（32 位） | 2026-11-05 | `Grand_LAES.flac`、`Grand_LAES.wav`、`03_LAES_demo.lae.wav` | 三个文件均能打开，并读取为 FLAC / PCM、44.1 kHz：即核心母带（LAES）或 CD 层（LAEY）。文件已加入播放列表，但未在 foobar2000 中播放（测试期间无屏幕访问权限） |
| foobar2000 2.26.2（64 位）和 1.5.1（32 位），安装 LAE 组件 0.16 | 2026-11-05 | `Grand_LAES.flac`；一个 LAES FLAC 片段 | 识别为 LAES：读出了母带列表和待播放的母带（352.8 kHz）。未播放（同上）；该组件使用与其他 LAE 插件相同的解码器 |
| VLC 3.0.24（64 位，便携版） | 2026-11-05 | `Grand_LAES.flac` | 播放核心母带；经 VLC 解码为文件后，与参考结果逐字节相同 |
| VLC 3.0.24 | 2026-11-05 | `Grand_LAES.wav`、`03_LAES_demo.lae.wav` | 播放核心母带（LAES）和 CD 层（LAEY）。经 VLC 解码为文件后与参考结果相同，但缺少最后 346 帧（8 ms）。普通 WAV 文件在 VLC 中缺少同样的帧；原因在于 VLC 自身的 WAV 处理路径，而非 LAE 数据 |
| VLC 3.0.24，安装 LAE 模块 0.1.1 | 2026-11-05 | 一个 LAES FLAC 片段 | 播放母带 `master_2_352800_32`；经 VLC 解码为文件后，除最后 40 ms 外与该母带逐字节相同（VLC 对流结尾的处理，原因同上） |
| Windows 11（版本 26200）Media Foundation：媒体播放器、电影和电视、资源管理器 | 2026-11-05 | `Grand_LAES.flac`、若干 LAES FLAC 片段 | 无法识别（限制 1）。普通 24 bit FLAC 文件能播放；普通 32 bit FLAC 文件能打开但不能解码 |
| Windows 11 资源管理器 | 2026-11-05 | `Grand_LAES.flac` | 时长、码率和标题均为空（限制 1） |
| Android 13 系统解码器（索尼 Xperia XQ-BC72），即将文件交由系统处理的应用所使用的解码器 | 2026-11-05 | 上表中的文件 | 见上表 |
| DirectShow 播放器（PotPlayer、MPC-BE、MPC-HC），安装 LAE 滤镜 0.2.1 | 2026-11-05 | `Grand_LAES.flac`；`03_LAES_demo.lae.wav` | 母带：352.8 kHz，609 s；跳转正常。对于 LAEY 文件，跳转正常，但播放在跳转点之后约 4 s 处才恢复，与 0.2.0 相同（直接通过滤镜本身测试，未使用播放器窗口） |

其他基于 libFLAC 或 FFmpeg 的播放器（例如 mpv 和大多数应用）预期与这两个库表现相同。只有软件实测记录中列出的应用程序作为应用程序进行过测试。

## 其他解码器的已知限制

1. **Windows 自带的 FLAC 解码器与 LAES FLAC。**
   - Windows（Media Foundation，媒体播放器和电影和电视应用使用它，资源管理器读取时长和标签时也使用它）仅在 FLAC 文件的前 4 MiB 内查找第一个音频帧。实测：第一个音频帧位于字节偏移 4,194,308（4 MiB + 4）或更早处时可被识别；自 4,194,309 起则无法识别。
   - 包含母带的 LAES FLAC 文件将母带存放在音频帧之前，这是 RFC 9639 对所有元数据的要求。因此其第一个帧的起始位置超出上述界限，Windows 无法识别该文件。
   - 该解码器也不解码 32 bit FLAC。尽管如此，LAES FLAC 文件仍是有效的 FLAC 文件：libFLAC 和 FFmpeg 均能播放（已使用 16 bit 和 24 bit 核心母带测试；见上表）。
   - **替代方法**：使用 WAV 形式、基于 libFLAC 或 FFmpeg 的播放器，或 LAE 插件。
2. **32 bit FLAC。**
   - 许多解码器不播放 32 bit FLAC。Android 13 的解码器无法识别它，Windows 的解码器能打开但不能解码。
   - 因此，LAES 参考编码器默认在 WAV 形式中写入 16 bit 核心母带，在 FLAC 形式中写入至多 24 bit 的核心母带；`--core-bits=32` 则生成 32 bit 核心母带。为获得最广泛的兼容性，请勿使用 `--core-bits=32`。
3. **RF64（超过 4 GiB 的 LAES WAV 形式）。**
   - RIFF WAV 文件不能超过 4 GiB，因此更大的 LAES WAV 文件（例如包含众多母带的长专辑）以 RF64 写入。
   - 许多 WAV 读取器不支持 RF64。Android 13 的解码器无法打开它。
   - **替代方法**：此类文件使用 FLAC 形式。LAEY 文件始终为普通 RIFF。
4. **`.lae` 扩展名。**
   - 按文件名选择解码器的播放器（例如 foobar2000、Windows 媒体播放器和许多手机应用）在未安装 LAE 插件时无法打开 `.lae` 文件。
   - 按内容识别的播放器会播放核心母带（LAEY 为 CD 层）。如需在大多数播放器中播放核心母带，请按文件的形式将其重命名为 `.wav` 或 `.flac`（受限制 1–3 的约束）。
5. **通过网络直接流式传输文件本身。**
   - 在 FLAC 形式中，核心母带的音频帧位于其他母带的数据之后。从文件开头通过网络读取文件的播放器，必须先加载全部母带才能发出第一个声音。
   - 用于流式传输时，请提供由 `laes_export` 生成的普通 FLAC（仅含一个版本：核心母带、某一母带或某一采样率档位，即经 LAES 提升变换降至较低采样率的母带）。
6. **跳转。**
   - LAES FLAC 文件按设计不含 SEEKTABLE。播放器通过搜索帧进行跳转；此方式可行，但在某些播放器中可能较慢。
   - WAV 文件的跳转不受影响。
7. **WAV 中的标签。**
   - LAES 将同一套标签写入两次，分别为 RIFF LIST/INFO 和 ID3v2.4（`id3 ` 块），均采用 UTF-8。
   - 大多数播放器读取 ID3 标签。按系统代码页读取 INFO 的旧版 Windows 程序可能将非 ASCII 文本显示为乱码。
   - FLAC 标签为 Vorbis 注释，按 FLAC 的要求采用 UTF-8。

## 播放器无法播放 LAE 文件时

- **文件名以 `.lae` 结尾**：按文件的形式将其重命名为 `.wav` 或 `.flac`，或安装 LAE 插件。
- **文件为 FLAC 文件且播放器为 Windows 自带播放器**：使用 WAV 形式、其他播放器或 LAE 插件（限制 1）。
- **文件为核心母带为 32 bit 的 FLAC 文件**：以 24 bit 编码核心母带（限制 2）。
- **文件为大于 4 GiB 的 WAV 文件**：该文件为 RF64。请使用 FLAC 形式（限制 3）。
- **均不属于上述情形**：使用 `laes verify` 或 `laes parts` 检查文件，并用 FFmpeg 尝试（`ffmpeg -i file -f null -`）。若 FFmpeg 能播放，则问题在于该播放器。
