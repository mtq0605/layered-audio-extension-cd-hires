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
- [菜单顺序：`--menu-order`](#menu-order---menu-order)
- [修改母带名称：`laes names`](#changing-master-names-laes-names)
- [加入母带：`laes add`](#adding-masters-laes-add)
- [提取母带写成新文件：`laes extract`](#extracting-masters-into-a-new-file-laes-extract)
- [校验：`laes verify`](#checking-laes-verify)
- [时间位置：`laes align`](#time-position-laes-align)
- [测试辅助命令](#test-helpers)
- [文字、文件名和退出码](#text-file-names-and-exit-codes)

## 编译

本工具由单个 C++17 源文件 `src/laes_tool.cpp` 及其头文件组成。各母带和核心母带必须在所有平台上逐位复现，因此编译时应采用严格的浮点语义。

Windows（Visual Studio 2022，x64 Native Tools 命令提示符）：

```
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /bigobj /D_USE_MATH_DEFINES /Fe:laes.exe src\laes_tool.cpp
```

Linux 和 macOS：源代码中包含 POSIX 代码路径（`g++ -O2 -std=c++17 -ffp-contract=off -pthread`），本项目的 CI 在 Linux 上（x86-64 和 ARM64，g++ 和 clang++）编译 `laes` 并运行 `laes selftest`；在 Linux 上不运行其他命令，也不在 macOS 上编译 `laes`。在其他命令在这些系统上测试之前，用于发布的文件应使用 Windows 版本制作。2026-11-14 在 Linux x64（WSL 中的 g++ 13.3）上运行了 `dec`、`verify`、`extract` 和 `add`：解码出的 WAV 文件，以及 `extract` 和 `add` 写出的文件，与 Windows 版本逐字节相同。在此日期之前的版本中，Linux 上的 `dec` 把 WAV 文件写在输出文件夹旁边，`extract` 和 `add` 失败。

`laes selftest` 使用合成信号运行内置回归测试。退出码 0 表示全部测试通过。

## 命令一览

| 命令 | 功能 |
|---|---|
| `laes enc <out> ...` | 由一个或多个母带创建 LAES 文件。 |
| `laes tag <in> <out> ...` | 在不重新编码的情况下，写出一个标签和图片不同的新文件。 |
| `laes names <in> <out> ...` | 在不重新编码的情况下，写出一个母带名称、菜单顺序、默认项或派生版本设置不同的新文件。 |
| `laes add <in> <out> <master> ...` | 向已有的 LAES 文件加入母带，结果写为新文件；原有部分逐字节不变。 |
| `laes extract <in> <out> --pick=...` | 把选中的一个或多个母带写成新的 LAES 文件。 |
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

- 母带为 16、24 或 32 位立体声 WAV 文件（RIFF，超过 4 GiB 时为 RF64）或 FLAC 文件，采样率为 44.1 或 48 kHz 的 1、2、4 或 8 倍。一个文件最多可包含 255 个母带。每个采样率、每种字长都可以放独立的母带，同一采样率和字长也可以有多个母带。
- 如果母带 WAV / RF64 或 `--core` WAV 的文件头给出的数据长度超出文件末尾，`laes enc` 和 `laes add` 会打印一条 `NOTE`，说明该文件可能不完整，然后读到文件末尾并继续运行（自 2026-11-14 起）。文件头中的数据长度为 2^63 字节或以上（RF64 的 `ds64`）时，给出原因并拒绝；`laes parts`、`tag`、`add`、`extract` 和 `names` 遇到 LAES 文件中这样的块长度时同样拒绝。
- 命令行中的顺序不影响文件大小：工具自行选择各母带在文件中的顺序和存储方式，取其中最小者。命令行中的顺序就是菜单顺序（见[菜单顺序](#menu-order---menu-order)）。
- `--name=<k>:<text>`：给命令行中的第 k 个母带起名称（从 1 起数；可重复使用，给其他母带起名称）。名称是发行方写的自由文本：UTF-8，最多 255 字节（约 255 个拉丁字母或 85 个汉字；可以用 emoji），不含控制字符（换行、制表符）。播放器原样显示；名称从不参与任何选择。工具不会把文件名、路径或日期自动当作名称。编码结束时打印"命令行第 k 个 -> 母带 n -> 名称"。
- `--no-derive`：完全不提供派生 16 位版本，母带和核心母带都没有。`--no-derive=<k>`：只有命令行中的第 k 个母带不提供。`--no-derive=core`：只有核心母带不提供。
- `--default=<k>`：听众没有选择时，播放器播放命令行中的第 k 个母带。`--default=core`，或者不写 `--default`：播放核心母带。默认项总是母带本身，不能是采样率档位或派生版本。播放器无法播放它时，播放核心母带。编码结束时打印 `default: given k -> master n in menus`。
- 有名称、有 `--no-derive`、有 `--default=<k>`、母带超过 16 个，或者菜单顺序与编码器选择的文件顺序不同时，文件写为有效载荷版本 2；2026-11-11 以前的播放器和工具对这样的文件只播放核心母带。其他情况下，文件写为有效载荷版本 1，与以前逐字节相同。
- 母带超过 16 个时，每个母带的参照先在 4 段固定的短片段上试编，只有最小的 3 个候选完整编码；文件顺序只试一种。在 10 秒片段上实测：20 个母带时，文件与完整搜索的结果逐字节相同；64 个母带约需 8 分钟。
- `<master>@T`：母带在核心母带时间轴上的时间位置，以基础采样率的样点为单位（十进制数，例如 `master.wav@-0.37`）。默认值为 0，对派生的核心母带而言即为正确值。
- `<master>@auto`：用于独立于给定核心母带制作的母带（例如已发行 CD 的高解析度母带）。工具采用与 `laes align` 相同的搜索方法，相对于 `--core` 测量 T；该搜索需要 `lae_enc` 位于 `laes` 旁边，或通过 `--lae-enc=<path>` 指定。若对齐误差高于 −60 dB，工具显示该误差并请求确认；`--accept` 表示不经询问直接接受。若不接受，则不写入任何内容。需满足两个条件：通过 `--core` 给出的核心母带为 16 位（CD 或 DVD）；待对齐的母带以 WAV 文件提供，因为搜索读取的是 WAV，FLAC 母带将被拒绝。派生的核心母带无需 `@auto`：其各母带均位于 T = 0。搜索通过系统的命令解释器运行 `lae_enc`，因此会被命令解释器改动的路径在搜索开始之前被拒绝，并说明原因：Windows 上是含 `%` 或 `"` 的路径，其他系统上是含 `$`、`` ` ``、`"` 或 `\` 的路径。请给文件改名，或手动给出 T（`<master>@<T>`）。

### 形式和文件名

| 文件名 | 形式 |
|---|---|
| `out.flac` | FLAC 形式：核心母带位于 FLAC 音频帧中，其他母带位于 `LAE2` APPLICATION 块中。 |
| `out.wav` | WAV 形式：核心母带位于 `data` 中，其他母带位于 `LAE ` 块中（超过 4 GiB 时为 RF64）。 |
| `out.lae` | 本格式自身的扩展名：`--form=flac`（默认）或 `--form=wav`。 |

- LAE 插件依据文件内容而非文件名识别 LAES 文件；文件名仅用于将文件分派给解码器。
- 对于 `.flac` 和 `.wav`，任何播放器均可播放核心母带。
- `.lae` 文件仅能由识别该扩展名的播放器（装有 LAE 插件）或检查文件内容的播放器打开。
- 输出文件名与某个输入（母带或 `--core` 文件）相同时，在写出任何内容之前即被拒绝（自 2026-11-14 起）。已有的 `<out>` 只有在控制台上两次回答 yes 后或给了 `--overwrite` 时（见"文字、文件名和退出码"），并且新文件完整写好后，才被替换。

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
| `--mem=<GB>` | 内存预算；默认 2 GB（内存较小的机器上更少）。仅决定每次处理的片段长度。必须是大于 0、不超过 1048576 的数。 |
| `--small-disk` | 所需磁盘空间大幅减少：各母带的最高采样率档位以及解码后的 FLAC 母带不予保存，而在读取时重新生成。速度较慢。 |
| `--force-disk` | 即使估算的磁盘需求超过可用空间，也开始运行。 |
| `--disk-stop` | 可用空间低于 2 GB 时停止，而不是等待（保留工作文件夹）。 |
| `--disk-stop-remove` | 可用空间低于 2 GB 时停止并删除工作文件夹（已完成的工作丢失）。 |
| `--timing` | 打印各阶段的用时和峰值内存，以及工作文件夹的最大大小。 |

- **工作文件夹：**中间数据写入输出文件旁边的 `<out>.laes.tmp`，并在结束时删除。那里建不了工作文件夹时（例如只读位置），改建在系统临时文件夹，工具会说明（自 2026-11-15 起）。同一条命令（共用同一个工作文件夹）不能同时运行两次：工作文件夹里有一个锁文件，第二次运行立刻停止（退出码 2），不改动任何东西。
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
- **上限：**VORBIS_COMMENT 块或 PICTURE 块会达到 16 MB，或 WAV 形式的 ID3v2 标签（文字和图片合计）会达到 256 MB 时，拒绝这些标签。
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
- 旧文件从不被修改。已有的 `<out>` 不会被悄悄替换（见"文字、文件名和退出码"）。
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
laes dec <in> <out dir> --pick=<n>[,<n> ...] | <a>-<b> | <rate>:<bits> | master:<k> | name:<text>
```

- 不带选项时，写出核心母带（`core.wav`）和每个母带（`master_<k>_<rate>_<bits>.wav`）。以 `-` 开头的输出文件夹名会被拒绝（自 2026-11-14 起），这样漏写文件夹时，选项不会被当成文件夹名。
- `--levels` 另外写出每个母带的每个采样率档位（`master_<k>_level_<rate>_<bits>.wav`）。采样率档位是母带在其采样率的二分之一、四分之一或八分之一下的版本，并且是无损的。
- `--list` 按 [SPEC.zh.md](SPEC.zh.md) 第 10.3 节的顺序列出所有选项并编号：先是核心母带，然后每个母带一组（母带本身，下面是它的采样率档位和派生版本，前面带"-"）。母带有名称时，母带、它的档位和派生版本都显示这个名称。`--list` 只读母带记录，不解码（自 2026-11-15 起）；SHA-256 校验在解码和播放母带时进行。退出码为 0，记录没有通过检查时为 2。
- `--pick` 仅写出所选项：
  - `<n>`：列表的第 n 项；多项用逗号隔开，例如 `--pick=2,6`；
  - `<a>-<b>`：列表的第 a 到第 b 项，例如 `--pick=6-12`；范围和单个编号可以混用，例如 `--pick=2-4,9`；
  - `<rate>:<bits>`：只匹配母带本身，不匹配采样率档位或派生版本。只有一个母带符合时直接写出；有多个符合时由你勾选（见下文"多个母带符合时"）；
  - `master:<k>`：母带 k；
  - `name:<text>`：名称完全相同的母带。有多个时同样由你勾选；一个都没有时拒绝。
- **派生 16 位母带：**每个高于 16 位的母带，在它自己的采样率和它的每个采样率档位上各有一个（`derived16_<rate>_from_<source>.wav`），显示为"由母带派生"，并带来源母带的名称；高于 16 位的核心母带也有一个。用 `--no-derive` 关掉派生版本的母带没有。派生母带按固定的 TPDF 规则（[SPEC.zh.md](SPEC.zh.md) 第 10.1 节）生成，因此每个解码器均可逐位复现。相对于该规则，它是无损的。相对于其所取自的母带，它不是无损的：该母带经 TPDF 抖动重新量化为 16 位，无法由其恢复。它是派生母带，而非原始母带；它从不顶替采样率和字长相同的独立母带，仅在被选中时写出。
- SHA-256 校验未通过的母带不予提供，其采样率档位和派生版本亦不予提供。核心母带和其他母带不受影响。
- 高于 16 位的输出写为 `WAVE_FORMAT_EXTENSIBLE`。
- **磁盘检查：**写出任何内容之前，把所选 WAV 文件的大小与输出文件夹所在磁盘的剩余空间比较。放不下时不写出任何内容，退出码为 2；加 `--force-disk` 时照样写出。
- **写入失败：**无法完整写出的 WAV 文件会被删除，并报告为 `<name>.wav: FAILED, the file was removed: <reason>`。最后一行随即说明并非每个文件都已写出，退出码为 2。

**多个母带符合时：勾选列表**

用采样率和字长（或名称）选择、对上多个母带时，工具不替你决定，而是列出这些母带让你勾选：

```
--pick: '176400:24' fits 2 masters. Tick the ones to write:
> [ ]    6  176400 Hz 24-bit  master 2 "原版母带"
  [ ]   12  176400 Hz 24-bit  master 3 "原版母带"
Up / Down: move   Space: tick   Enter: done   Esc: cancel
Or type numbers or ranges (e.g. 6,12 or 6-12), then Enter to tick them:
Numbers:
```

- 每行显示勾选框、编号、采样率、字长和母带名称。
- **用按键勾选：**上下键移动，空格勾选或取消，回车写出勾选的母带。Esc 取消，什么都不写出。
- **在列表下方输入：**在 `Numbers:` 后输入编号或范围，然后按回车。
  - 写法：一个编号 `6`；多个用逗号隔开 `6,12`；用连字符表示范围 `6-12`，即 6 到 12（含两端）；混用，例如 `6,8-12`。
  - 回车勾选编号在输入内容里的候选，并取消其他候选。不是候选的编号不起作用。
  - 再按一次回车，写出勾选的母带。
  - 输入有误（例如 `7-`）时显示错误，清空这一行，可以重新输入；退格键删除一个字符。
- 一个都没有勾选就按回车时，会提示至少勾选一个。
- 勾选列表只在控制台窗口中出现。在脚本中（输入或输出被重定向），工具不等待：列出候选（编号、采样率、字长和名称相同），什么都不写出，以退出码 1 结束。请用 `--pick=<n>`、`--pick=<n>,<n>` 或 `--pick=<a>-<b>` 给出。

## 菜单顺序：`--menu-order`

文件中母带的顺序仍由编码器选择（为了文件最小）；菜单和选择中的顺序由你标记。两种顺序互不相关。

- `laes enc`：不加选项时，菜单顺序就是母带在命令行中给出的顺序；要用别的顺序，写 `--menu-order=3,1,2`（数字指按给出顺序的母带）。
- 菜单顺序与文件顺序相同时，不存储任何内容，文件可以是版本 1；否则存储菜单顺序，文件是版本 2（2026-11-11 以前的播放器只播放核心母带）。
- 菜单顺序不决定默认项；默认项用 `--default` 设定（否则是核心母带）。`--list`、`--pick=master:<k>`、解码出的文件名 `master_<k>_…` 以及 `laes names` 的 k 都使用菜单编号。

## 修改母带名称：`laes names`

```
laes names <in> <out> [--name=<k>:<text> ...] [--no-derive[=<k>|core]] [--derive[=<k>|core]] [--menu-order=<k1>,<k2>,...] [--default=<k>|core]
```

- 不重新编码，写出一个新文件；原文件保留，已有的 `<out>` 不会被悄悄替换（见"文字、文件名和退出码"）。
- k 是母带当前在菜单中的编号（`laes dec --list` 中的"master k"）。`--name=<k>:` 后面不写文本表示去掉名称；`--derive` 重新打开派生版本；`--menu-order=` 按当前编号给出新的菜单顺序；`--default=<k>` 或 `--default=core` 设定默认项；`--no-derive=core` 或 `--derive=core` 关掉或打开核心母带的派生版本。
- `laes names <in> --in-place --default=<k>|core [--no-derive=core | --derive=core]` 直接修改文件本身，不写新文件。只允许这几个选项，因为它们不改变文件大小：最多改动 7 个字节（有效载荷版本、头部的两个字节和 CRC-32）。工具打印每个改动的字节及其原值和新值，然后解码每个母带并核对 SHA-256；任何一项失败都写回原来的字节。结果与用相同选项直接编码写出的文件逐字节相同。
- 各层、核心母带和标签逐字节复制；只有母带记录和 CRC-32 改变。之后每个母带都按 SHA-256 核对。
- 去掉所有名称和标志、母带不超过 16 个时，文件又是有效载荷版本 1，与直接编码写出的文件逐字节相同。

## 加入母带：`laes add`

```
laes add <in> <out> <new master>[@T] ... [--name=<k>:<text>] [--no-derive[=<k>]] [--menu-order=<k1>,<k2>,...]
```

- 写出一个新文件：核心母带以及每个原有母带的记录和各层逐字节复制，不重新编码。新母带放在有效载荷末尾；每个新母带相对核心母带和它前面的每个母带编码，取最小的结果（与 `laes enc` 相同）。
- `--name` 和 `--no-derive` 的 k 指按给出顺序的新母带。默认情况下，新母带排在菜单末尾；`--menu-order=` 给出全部母带的菜单顺序：原有母带用当前编号，新母带按给出顺序接在后面编号。
- 原有母带解码一次（它们是新母带的参照）；之后每个母带都按 SHA-256 核对。标签、图片和核心母带的音频字节保持不变。
- 支持 4 GiB 以上（RF64）的核心母带。开始之前，`laes add` 先估算所需的磁盘空间；超过剩余空间时会说明，然后继续运行。

## 提取母带写成新文件：`laes extract`

```
laes extract <in> <out> --pick=<p>[,<p> ...]      p = master:<k> | name:<text> | <rate>:<bits>
```

- 写出一个只含所选母带（一个或多个）的新文件。`<rate>:<bits>` 和 `name:` 取符合的母带；有多个符合时，在与 `laes dec` 相同的勾选列表中勾选（这里的编号是 `master:<k>` 的 k）。`master:<a>-<b>` 取母带 a 到 b。
- 所选母带完整解码后重新编码（用同一个核心母带），各自在保留的母带中选择参照；不从原有效载荷中截取任何内容。
- `--pick` 中的顺序就是新文件的菜单顺序；名称和"不提供派生版本"的设置随母带一起带过去；标签、图片和核心母带的音频字节保持不变。之后每个母带都按 SHA-256 核对。
- 每个母带的时间位置 T 都原样保留：它作为数值交给编码器，而不是文字（自 2026-11-15 起）。开始之前，`laes extract` 先估算所需的磁盘空间；超过剩余空间时会说明，然后继续运行。
- 只要 WAV 文件、不需要封装时，用 `laes dec --pick=...`。

## 校验：`laes verify`

```
laes verify <in> <master.wav> [<master.wav> ...]
```

解码文件中的每个母带，并与给定文件逐位比较，给定文件的顺序不限。对每个文件打印 `IDENTICAL` 或 `DIFFERENT`；对于没有对应文件的母带，打印 `not compared`。无法打开或被 WAV 读取器拒绝的给定文件，报告为 `<file>: DIFFERENT (not compared: <reason>)`，其后的文件照常比较（自 2026-11-14 起）。`laes verify` 和 `laes dec` 用菜单上的编号称呼每个母带（文件的菜单顺序），和 `--pick=master:<k>` 用的编号相同（自 2026-11-15 起）。流之前带 ID3v2 标签或流之后带 APEv2 / ID3v1 标签（其他程序写入的标签）的 FLAC 文件，作为母带和作为 LAES 文件都接受；这些标签不读取，留在文件里，`verify`、`dec` 和 `parts` 会报告有这些标签。

## 时间位置：`laes align`

```
laes align <cd.wav> <master.wav> [--lae-enc=<path>] [--accept]
```

对于独立制作的核心母带（例如已发行的 CD），本命令测量母带在该 CD 时间轴上的时间位置 T，包括小数部分。它使用 LAE Lossy 编码器（`lae_enc`，位于 `laes` 旁边或通过 `--lae-enc` 指定）的自动搜索。会被命令解释器改动的路径被拒绝，与 `@auto` 相同。对齐误差为 −60 dB 或更低时通过；否则显示该误差并由用户决定，或由 `--accept` 不经询问直接接受。母带静音而核心母带不静音的检查段算作没对上（自 2026-11-15 起）；工具说明是哪一段，并问是否采用这个对齐。本命令不写入任何内容；对齐被接受时，打印 `use <master>@<T>` 供 `laes enc` 使用（退出码 0）。整张专辑由 LAE Lossy 的整盘规划器（`lae_enc plan … laes`）切分为音轨。

## 测试辅助命令

| 命令 | 功能 |
|---|---|
| `laes selftest` | 使用合成信号运行回归测试；退出码 0 表示全部测试通过。 |
| `laes excerpt <in.wav> <out.wav> <start s> <seconds> <bits> [pad s]` | 截取片段；16 = TPDF 降至 16 位，24 = 保持原样，32 = 低字节随机。 |
| `laes reduce <in.wav> <out.wav> <bits>` | 以 TPDF 抖动和固定种子降低字长。 |
| `laes mkcd <master.wav> <cd.wav> <tau> [gain] [noise]` | 创建测试用"CD"：16 位，母带的基础档位平移 tau。 |

## 文字、文件名和退出码

- **文字：**在 Windows 上，所有参数、文件名、标签和控制台输出在工具内部均以 UTF-8 处理。无论系统代码页为何，在 cmd、PowerShell 和脚本中均可使用任何语言的标签和路径。在其他系统上，参数按原样接收（在常见系统上为 UTF-8）。
- **退出码：**0：成功；1：参数无效；2：操作失败（文件无法读取或写入、校验未通过或磁盘已满）。退出码为 2 时，`laes enc` 和 `laes tag` 均未写出 `<out>`（中途停止的 `laes enc` 可保留其工作文件夹 `<out>.laes.tmp` 以便续传）。
- **已有的输出文件：**已有的输出文件不会被悄悄替换（`enc`、`dec`、`tag`、`add`、`extract`、`names` 和 `laes_export`）。给了 `--overwrite` 时不询问，直接替换。没有给时，工具在控制台上问两次，两次都回答 yes 才替换；不在控制台上，或者回答的不是 yes，就以退出码 1 拒绝。`laes enc` 接着做上次没做完的工作不算替换。提示的原文见 [README.zh.md](../../README.zh.md) 第 14.11 节。
- **测试开关：**`laes` 和 `laes_export` 的测试开关（`LAES_TEST_KEYS`、`LAES_NO_SHORTLIST`、`LAES_FORCE_RF64`、`LAES_PIECE`、`LAES_SELFCHECK_FLIP`、`LAES_DISK_RESERVE`）只在设了 `LAE_TEST_MODE=1` 时起作用；这样运行时打印一行，列出起作用的开关。`KEEPTMP`（保留工作文件夹，不改输出的任何字节）不设它也起作用（[README.zh.md](../../README.zh.md) 第 14.11 节）。
- **提示信息**为英文；磁盘和续传相关的提示、对已有输出文件的询问，以及工作文件夹改建在系统临时文件夹的说明，另含一行中文。
