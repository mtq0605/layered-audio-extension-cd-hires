# laes：LAE Lossless 命令行工具

[English](LAES_TOOL.md) | **中文**（以英文版为准）

`laes` 用于创建、读取和校验 LAES 文件（LAE Lossless，ID `LAE2`）。LAES 文件是普通的 FLAC 或 WAV 文件。文件中包含一个所有播放器均可播放的**核心母带**，以及一个或多个其他**无损母带**；支持 LAE 的播放器和 `laes` 可逐位解码这些无损母带。

- LAES 的介绍见 [OVERVIEW.zh.md](OVERVIEW.zh.md)。
- 文件格式见 [SPEC.zh.md](SPEC.zh.md)。本指南仅介绍参考工具。

## 目录

- [编译](#building)
- [命令一览](#commands-at-a-glance)
- [制作文件：`laes enc`](#making-a-file-laes-enc)
- [标签和图片](#tags-and-pictures)
- [修改标签：`laes tag`](#changing-tags-laes-tag)
- [查看内部结构：`laes parts`](#looking-inside-laes-parts)
- [解码：`laes dec`](#decoding-laes-dec)
- [校验：`laes verify`](#checking-laes-verify)
- [时间位置：`laes align`](#time-position-laes-align)
- [测试辅助命令](#test-helpers)
- [文字、文件名和退出码](#text-file-names-and-exit-codes)

## 编译

本工具由单个 C++17 源文件 `src/laes_tool.cpp` 及其头文件组成。各母带和核心母带必须在所有平台上逐位复现，因此编译时应采用严格的浮点语义。

Windows（Visual Studio 2022，x64 Native Tools 命令提示符）：

```
cl /nologo /O2 /EHsc /std:c++17 /fp:strict /bigobj /D_USE_MATH_DEFINES /Fe:laes.exe src\laes_tool.cpp
```

Linux 和 macOS：源代码中包含 POSIX 代码路径（`g++ -O2 -std=c++17 -ffp-contract=off -pthread`），但本项目的 CI 尚未在这些系统上编译或测试 `laes`。在此之前，用于发布的文件应使用 Windows 版本制作。

`laes selftest` 使用合成信号运行内置回归测试。退出码 0 表示全部测试通过。

## 命令一览

| 命令 | 功能 |
|---|---|
| `laes enc <out> ...` | 由一个或多个母带创建 LAES 文件。 |
| `laes tag <in> <out> ...` | 在不重新编码的情况下，写出一个标签和图片不同的新文件。 |
| `laes parts <file>` | 显示各个块、音频字节和有效载荷的 SHA-256，以及标签。 |
| `laes dec <in> <dir> ...` | 将核心母带、各母带及其他版本写为 WAV 文件。 |
| `laes verify <in> <master.wav> ...` | 解码每个母带，并与给定文件逐位比较。 |
| `laes align <cd.wav> <master.wav>` | 测量母带在已发行 CD 时间轴上的位置。 |
| `laes selftest` | 运行回归测试。 |
| `laes excerpt`、`laes reduce`、`laes mkcd` | 测试辅助命令。 |

不带参数运行 `laes` 可显示上述列表。

## 制作文件：`laes enc`

```
laes enc <out.flac | out.wav | out.lae> (--derive-core | --core=<core.wav>) <master> [<master> ...] [options]
```

### 核心母带

- `--derive-core`：核心母带由各母带派生得到，取某一母带的基础档位（44.1 或 48 kHz），字长由 `--core-bits` 设定。
  - 默认字长：WAV 形式为 16 位；FLAC 形式取各母带中最长的字长，最大为 24 位。
  - `--core-bits=16|24|32` 可在两种形式中选择其他字长（FLAC 形式同样允许 32 位核心母带；各播放器的情况见 [COMPATIBILITY.zh.md](../COMPATIBILITY.zh.md)）。若某一母带的字长恰好等于该值，则直接以其基础档位作为核心母带，不进行重新量化；否则，对字长最长的母带施加 TPDF 抖动降低字长。具体规则见 [SPEC.zh.md](SPEC.zh.md) 第 11.1 节，任何人均可据此核对。
- `--core=<core.wav>`：直接提供核心母带，例如已发行 CD 或 DVD 某一音轨的 16 位 WAV。此时，独立于该核心母带制作的母带需要给出其在该核心母带上的时间位置（见下文）。

### 母带

- 母带为 16、24 或 32 位立体声 WAV 文件（RIFF，超过 4 GiB 时为 RF64）或 FLAC 文件，采样率为 44.1 或 48 kHz 的 1、2、4 或 8 倍。一个文件最多可包含 16 个母带。
- 命令行中的顺序无关紧要：工具自行选择各母带的顺序和存储方式，取其中最小者。
- `<master>@T`：母带在核心母带时间轴上的时间位置，以基础采样率的样点为单位（十进制数，例如 `master.wav@-0.37`）。默认值为 0，对派生的核心母带而言即为正确值。
- `<master>@auto`：用于独立于给定核心母带制作的母带（例如已发行 CD 的高解析度母带）。工具采用与 `laes align` 相同的搜索方法，相对于 `--core` 测量 T；该搜索需要 `lae_enc` 位于 `laes` 旁边，或通过 `--lae-enc=<path>` 指定。若对齐误差高于 −60 dB，工具显示该误差并请求确认；`--accept` 表示不经询问直接接受。若不接受，则不写入任何内容。需满足两个条件：通过 `--core` 给出的核心母带为 16 位（CD 或 DVD）；待对齐的母带以 WAV 文件提供，因为搜索读取的是 WAV，FLAC 母带将被拒绝。派生的核心母带无需 `@auto`：其各母带均位于 T = 0。

### 形式和文件名

| 文件名 | 形式 |
|---|---|
| `out.flac` | FLAC 形式：核心母带位于 FLAC 音频帧中，其他母带位于 `LAE2` APPLICATION 块中。 |
| `out.wav` | WAV 形式：核心母带位于 `data` 中，其他母带位于 `LAE ` 块中（超过 4 GiB 时为 RF64）。 |
| `out.lae` | 本格式自身的扩展名：`--form=flac`（默认）或 `--form=wav`。 |

- LAE 插件依据文件内容而非文件名识别 LAES 文件；文件名仅用于将文件分派给解码器。
- 对于 `.flac` 和 `.wav`，任何播放器均可播放核心母带。
- `.lae` 文件仅能由识别该扩展名的播放器（装有 LAE 插件）或检查文件内容的播放器打开。

### 示例

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_352k_24.wav Piece_176k_24.wav
laes enc Piece_LAES.wav  --derive-core Piece_352k_24.wav
laes enc Track01_LAES.wav --core=Track01_cd.wav Track01_master.wav@auto --accept
laes enc Piece_LAES.lae --form=wav --derive-core Piece_352k_24.wav --tag=TITLE=Piece
```

### 写入前的检查

文件首先写为 `<out>.part`，然后完全按照播放器的读取方式读回。工具检查文件结构、有效载荷的 CRC-32 以及核心母带；解码每个母带，并与其输入逐个样点比较，同时核对为每个母带存储的 SHA-256。仅当全部一致时，才将 `<out>.part` 重命名为 `<out>`；否则将其删除，不写入任何内容。

### 内存、磁盘和续传

以下选项均不改变所生成的文件：相同的母带和选项产生相同的字节。

| 选项 | 作用 |
|---|---|
| `--mem=<GB>` | 内存预算；默认 2 GB（内存较小的机器上更少）。仅决定每次处理的片段长度。 |
| `--small-disk` | 所需磁盘空间大幅减少：各母带的最高采样率档位以及解码后的 FLAC 母带不予保存，而在读取时重新生成。速度较慢。 |
| `--force-disk` | 即使估算的磁盘需求超过可用空间，也开始运行。 |
| `--disk-stop` | 可用空间低于 2 GB 时停止，而不是等待（保留工作文件夹）。 |
| `--disk-stop-remove` | 可用空间低于 2 GB 时停止并删除工作文件夹（已完成的工作丢失）。 |
| `--timing` | 打印各阶段的用时和峰值内存，以及工作文件夹的最大大小。 |

- **工作文件夹：**中间数据写入输出文件旁边的 `<out>.laes.tmp`，并在结束时删除。
- **磁盘检查：**开始前，工具估算所需的磁盘空间。若该估算值加上工具保留的 2 GB 超过可用空间，则不开始运行（`--force-disk` 使其仍然开始）。运行期间，若可用空间低于 2 GB，工具将等待直至再次有可用空间，除非给出了 `--disk-stop` 或 `--disk-stop-remove`。
- **续传：**中途停止的运行（因磁盘已满，或工具被关闭或崩溃）保留其工作文件夹。再次运行同一命令将从停止处继续，结果与不间断运行相同。若参数、某一母带、`--core` 文件或工具本身发生变化，则从头开始运行；`--small-disk` 的变化同样如此。标签选项、`--mem`、`--timing`、`--force-disk`、`--disk-stop` 和 `--disk-stop-remove` 不在考虑之列，续传前可更改。
- **实测的用时、内存和磁盘用量**（6、10 和 30 分钟，2 至 12 个母带）见 [SPEC.zh.md](SPEC.zh.md) 第 11.5 节。在这些运行中，峰值内存均低于 2 GB 的预算。工作文件夹随时长以及母带的数量和采样率增长：30 分钟、12 个母带时为 27.8 GB，使用 `--small-disk` 时为 15.0 GB。

## 标签和图片

`laes enc` 和 `laes tag` 接受相同的选项。若未给出其中任何一项，则不写入标签，所生成的文件与不支持标签时制作的文件逐字节相同。

| 选项 | 含义 |
|---|---|
| `--tag=KEY=VALUE` | 一个标签。KEY 为 Vorbis comment 字段名，例如 `TITLE`、`ARTIST`、`ALBUM`、`ALBUMARTIST`、`DATE`、`GENRE`、`TRACKNUMBER`、`DISCNUMBER`、`COMPOSER`、`COPYRIGHT`、`COMMENT` 或 `ISRC`。同一字段名可给出多次。 |
| `--cover=<image>` | 封面。 |
| `--picture=<type>:<image>` | 其他图片，数量不限：4 封底、5 内页、6 介质（碟面）、7 主要艺人、8 艺人……（ID3v2 和 FLAC 的图片类型，0 至 20）。 |

- **图片：**JPEG 或 PNG，逐字节原样存储（从不重新编码）。无最小尺寸要求。单张图片最大约 16 MB（FLAC 单个元数据块的上限；WAV 形式同样适用该上限），全部图片合计最大 256 MB。图片过大会使部分手机播放器打开文件变慢。
- **文字：**UTF-8，因此可使用任何语言。字段名本身为 ASCII。
- **标签的位置：**一组标签描述核心母带和所有母带。

| 形式 | 标签 | 图片 | 位置 |
|---|---|---|---|
| FLAC | 一个 `VORBIS_COMMENT` 块 | 每张图片一个 `PICTURE` 块 | STREAMINFO 之后、`LAE2` 块之前 |
| WAV | 一个 `LIST`/`INFO` 块和一个 `id3 ` 块（ID3v2.4） | `id3 ` 块中的 `APIC` 帧 | `fmt ` 之后、`data` 之前 |

- WAV `INFO` 仅支持部分字段：`TITLE` → `INAM`、`ARTIST` → `IART`、`ALBUM` → `IPRD`、`DATE` → `ICRD`、`COPYRIGHT` → `ICOP`、`COMMENT` → `ICMT`、`GENRE` → `IGNR`、`TRACKNUMBER` → `ITRK`。所有字段均写入 `id3 `；没有专用 ID3 帧的字段写入 `TXXX`。
- 按系统代码页读取 WAV `INFO` 的旧软件会错误显示非 ASCII 文字。`id3 ` 始终为 UTF-8，同时读取两者的播放器应优先使用 `id3 `。
- 写入标签时，工具在标签中以 `laes (LAE Lossless)` 标识自身，不带版本号，从而使相同的输入始终产生相同的文件。
- 标签从不参与校验：每个母带的 SHA-256 仅覆盖样点，且标签从不决定播放哪个版本。

示例：

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_176k_24.wav "--tag=TITLE=Piece" "--tag=ARTIST=Name" "--tag=DATE=2026" "--tag=COPYRIGHT=(c) 2026 Name. All rights reserved." --cover=cover.jpg --picture=4:back.jpg
```

## 修改标签：`laes tag`

```
laes tag <in> <out> [--tag=KEY=VALUE ...] [--cover=<image>] [--picture=<type>:<image> ...]
```

- 写出一个**新**文件，其标签和图片恰为所给出的那些。若无标签选项，新文件完全不含标签。
- 旧标签被丢弃。音频字节、有效载荷以及其他所有块均逐字节复制。
- 结果与使用相同的选项和相同版本的工具、带这些标签重新编码相同母带所得的文件逐字节相同。
- 随后对新文件进行完整解码，每个母带均与其存储的 SHA-256 核对；音频字节和有效载荷还与旧文件中的相应内容比较。任何差异均导致新文件被删除。
- 旧文件从不被修改。若 `<out>` 已存在，则不执行任何操作。
- 支持 FLAC 形式和 WAV 形式（包括 RF64）。LAEY 文件（`LAE1`）使用 `lae_enc tag` 修改标签。

## 查看内部结构：`laes parts`

```
laes parts <file>
```

列出 FLAC 元数据块或 RIFF 块及其大小。打印音频字节（FLAC 帧或 WAV `data`）和有效载荷的 SHA-256，以及播放器读取到的标签。两个 SHA-256 值均相同的两个文件包含相同的音频和相同的母带，与其标签无关。

## 解码：`laes dec`

```
laes dec <in> <out dir> [--levels]
laes dec <in> --list
laes dec <in> <out dir> --pick=<n>[,<n> ...] | --pick=<rate>:<bits>
```

- 不带选项时，写出核心母带（`core.wav`）和每个母带（`master_<k>_<rate>_<bits>.wav`）。
- `--levels` 另外写出每个母带的每个采样率档位（`master_<k>_level_<rate>_<bits>.wav`）。采样率档位是母带在其采样率的二分之一、四分之一或八分之一下的版本，并且是无损的。
- `--list` 列出所有选项并编号。`--pick` 仅写出所选项，可按编号或按采样率和字长选择。
- **派生 16 位母带：**对于没有自身 16 位选项的每个采样率，提供一个 16 位版本（`derived16_<rate>_from_<name>.wav`）。该版本由该采样率下字长最长的选项按固定的 TPDF 规则（[SPEC.zh.md](SPEC.zh.md) 第 10.1 节）生成，因此每个解码器均可逐位复现。相对于该规则，它是无损的。相对于其来源选项，它不是无损的：该来源选项经 TPDF 抖动重新量化为 16 位，无法由其恢复。它是派生母带，而非原始母带，仅在被选中时写出。
- SHA-256 校验未通过的母带不予提供，其采样率档位亦不予提供。核心母带和其他母带不受影响。
- 高于 16 位的输出写为 `WAVE_FORMAT_EXTENSIBLE`。

## 校验：`laes verify`

```
laes verify <in> <master.wav> [<master.wav> ...]
```

解码文件中的每个母带，并与给定文件逐位比较，给定文件的顺序不限。对每个文件打印 `IDENTICAL` 或 `DIFFERENT`；对于没有对应文件的母带，打印 `not compared`。

## 时间位置：`laes align`

```
laes align <cd.wav> <master.wav> [--lae-enc=<path>] [--accept]
```

对于独立制作的核心母带（例如已发行的 CD），本命令测量母带在该 CD 时间轴上的时间位置 T，包括小数部分。它使用 LAE Lossy 编码器（`lae_enc`，位于 `laes` 旁边或通过 `--lae-enc` 指定）的自动搜索。对齐误差为 −60 dB 或更低时通过；否则显示该误差并由用户决定，或由 `--accept` 不经询问直接接受。本命令不写入任何内容；对齐被接受时，打印 `use <master>@<T>` 供 `laes enc` 使用（退出码 0）。整张专辑由 LAE Lossy 的整盘规划器（`lae_enc plan … laes`）切分为音轨。

## 测试辅助命令

| 命令 | 功能 |
|---|---|
| `laes selftest` | 使用合成信号运行回归测试；退出码 0 表示全部测试通过。 |
| `laes excerpt <in.wav> <out.wav> <start s> <seconds> <bits> [pad s]` | 截取片段；16 = TPDF 降至 16 位，24 = 保持原样，32 = 低字节随机。 |
| `laes reduce <in.wav> <out.wav> <bits>` | 以 TPDF 抖动和固定种子降低字长。 |
| `laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]` | 创建测试用"CD"：16 位，母带的基础档位平移 tau。 |

## 文字、文件名和退出码

- **文字：**在 Windows 上，所有参数、文件名、标签和控制台输出在工具内部均以 UTF-8 处理。无论系统代码页为何，在 cmd、PowerShell 和脚本中均可使用任何语言的标签和路径。在其他系统上，参数按原样接收（在常见系统上为 UTF-8）。
- **退出码：**0：成功；1：参数无效；2：操作失败（文件无法读取或写入、校验未通过或磁盘已满）。退出码为 2 时，`laes enc` 和 `laes tag` 均未写出 `<out>`（中途停止的 `laes enc` 可保留其工作文件夹 `<out>.laes.tmp` 以便续传）。若 `<out>` 已存在，`laes tag` 以退出码 1 停止。
- **提示信息**为英文；磁盘和续传相关的提示信息另含一行中文。
