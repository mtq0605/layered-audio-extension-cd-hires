# laes：LAE 无损命令行工具

[English](LAES_TOOL.md) | **中文**

`laes` 用来制作、读取和校验 LAES 文件（LAE 无损，ID `LAE2`）。LAES 文件就是普通的 FLAC 或 WAV 文件，里面有一个所有播放器都能播的**核心母带**，以及一个或多个其他**无损母带**；支持 LAE 的播放器和 `laes` 能把这些母带逐位解出来。

- LAES 是什么：[OVERVIEW.zh.md](OVERVIEW.zh.md)。
- 文件格式：[SPEC.zh.md](SPEC.zh.md)。本文只介绍参考工具。

## 目录

- [编译](#编译)
- [命令一览](#命令一览)
- [制作文件：`laes enc`](#制作文件laes-enc)
- [标签和图片](#标签和图片)
- [修改标签：`laes tag`](#修改标签laes-tag)
- [查看结构：`laes parts`](#查看结构laes-parts)
- [解码：`laes dec`](#解码laes-dec)
- [校验：`laes verify`](#校验laes-verify)
- [时间位置：`laes align`](#时间位置laes-align)
- [测试辅助命令](#测试辅助命令)
- [文字、文件名和退出码](#文字文件名和退出码)

## 编译

工具只有一个 C++17 源文件（`src/laes_tool.cpp`）和它的头文件。核心母带和各个母带在所有平台上都必须逐位一致，所以要用严格浮点编译。

Windows（Visual Studio 2022，x64 Native Tools 命令行）：

```
cl /nologo /O2 /EHsc /std:c++17 /fp:strict /bigobj /D_USE_MATH_DEFINES /Fe:laes.exe src\laes_tool.cpp
```

Linux / macOS：源码里有 POSIX 的代码路径（`g++ -O2 -std=c++17 -ffp-contract=off -pthread`），但项目的 CI 还没有在这些系统上编译和测试 `laes`。在此之前，要发布的文件请用 Windows 版制作。

`laes selftest` 用合成信号跑内置的回归测试，退出码 0 表示全部通过。

## 命令一览

| 命令 | 作用 |
|---|---|
| `laes enc <输出> ...` | 用一个或多个母带制作 LAES 文件 |
| `laes tag <输入> <输出> ...` | 不重新编码，写一个换了标签和图片的新文件 |
| `laes parts <文件>` | 显示各个块、音频字节和载荷的 SHA-256，以及标签 |
| `laes dec <输入> <文件夹> ...` | 把核心母带、各个母带和其他版本写成 WAV 文件 |
| `laes verify <输入> <母带.wav> ...` | 解码并把每个母带和给出的文件逐位比较 |
| `laes align <cd.wav> <母带.wav>` | 测量母带在已发行 CD 时间轴上的位置 |
| `laes selftest` | 跑回归测试 |
| `laes excerpt`、`laes reduce`、`laes mkcd` | 测试辅助命令 |

不带参数运行 `laes` 会列出这些命令。

## 制作文件：`laes enc`

```
laes enc <out.flac | out.wav | out.lae> (--derive-core | --core=<core.wav>) <母带> [<母带> ...] [选项]
```

### 核心母带

- `--derive-core`：从母带派生核心母带，取某个母带的基础级别（44.1 或 48 kHz），字长为 `--core-bits`。
  - 默认字长：WAV 形式 16 位；FLAC 形式取母带中最长的字长，最多 24 位。
  - `--core-bits=16|24|32` 可以另选。如果有字长正好相同的母带，核心母带就是它的基础级别，一位不改；否则取字长最长的母带，用 TPDF 抖动降下来。准确的规则见 [SPEC.zh.md](SPEC.zh.md) 第 11.1 节，任何人都能核对。
- `--core=<core.wav>`：直接给出核心母带，例如已发行 CD 或 DVD 某一轨的 16 位 WAV。和它分开制作的母带，需要给出在这个核心母带上的时间位置（见下）。

### 母带

- 母带是 16、24 或 32 位立体声 WAV（RIFF，超过 4 GB 用 RF64），或者 FLAC 文件，采样率是 44.1 或 48 kHz 的 1、2、4 或 8 倍。每个文件最多 16 个母带。
- 命令行里的顺序没有关系：工具自己选择顺序和每个母带的存法，取最小的。
- `<母带>@T`：母带在核心母带时间轴上的时间位置，单位是基础采样率的样点（十进制，例如 `master.wav@-0.37`）。默认 0，派生核心母带时就是 0。
- `<母带>@auto`：用于和给定的核心母带分开制作的独立母带（例如已发行 CD 的高解析母带）。用和 `laes align` 相同的搜索，相对 `--core` 测量 T。对齐误差超过 −60 dB 时，工具显示误差并询问；加 `--accept` 不询问直接接受。不接受就什么都不写。两个条件：用 `--core` 给出的核心母带是 16 位（CD 或 DVD）；要对齐的那个母带以 WAV 文件给出，因为搜索只读 WAV，以 FLAC 给出的母带会被拒绝。派生的核心母带不需要 `@auto`：它的母带 T 都是 0。

### 形式和文件名

| 文件名 | 形式 |
|---|---|
| `out.flac` | FLAC 形式：核心母带在 FLAC 音频帧里，其他母带在 `LAE2` APPLICATION 块里 |
| `out.wav` | WAV 形式：核心母带在 `data` 里，其他母带在 `LAE ` 块里（超过 4 GB 用 RF64） |
| `out.lae` | 格式自己的扩展名：`--form=flac`（默认）或 `--form=wav` |

- 播放器按文件内容识别 LAES 文件，不看文件名。
- `.flac` 和 `.wav` 在任何播放器里都能播核心母带。
- `.lae` 只有认识这个扩展名（装了 LAE 插件）或会检查文件内容的播放器才能打开。

### 示例

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_352k_24.wav Piece_176k_24.wav
laes enc Piece_LAES.wav  --derive-core Piece_352k_24.wav
laes enc Track01_LAES.wav --core=Track01_cd.wav Track01_master.wav@auto --accept
laes enc Piece_LAES.lae --form=wav --derive-core Piece_352k_24.wav --tag=TITLE=Piece
```

### 写入前的检查

文件先写成 `<out>.part`，然后完全按播放器的方式读回来：检查文件结构、载荷的 CRC-32 和核心母带，解码每个母带并逐个样点和输入比较，同时核对每个母带存的 SHA-256。全部一致才把 `<out>.part` 改名为 `<out>`；否则删掉，什么都不写。

### 内存、磁盘和续跑

这些选项都不改变写出的文件：同样的母带和选项，得到同样的字节。

| 选项 | 作用 |
|---|---|
| `--mem=<GB>` | 内存预算，默认 2 GB（内存小的电脑更少）。只决定每次处理的片段长度。 |
| `--small-disk` | 磁盘用量小得多：每个母带最高的采样率级别和解码后的 FLAC 母带不保存，用到时重新生成。速度慢一些。 |
| `--force-disk` | 估算的磁盘需求超过剩余空间时也开始 |
| `--disk-stop` | 剩余空间低于 2 GB 时停下（保留工作文件夹），而不是等待 |
| `--disk-stop-remove` | 剩余空间低于 2 GB 时停下并删除工作文件夹（已做的工作丢失） |
| `--timing` | 打印每个阶段的用时和峰值内存，以及工作文件夹的最大用量 |

- **工作文件夹：**中间数据放在输出旁边的 `<out>.laes.tmp` 里，结束时删除。
- **磁盘检查：**开始前，工具估算需要的磁盘；这个数加上要保留的 2 GB 超过剩余空间，就不开始（加 `--force-disk` 照样开始）。运行中剩余空间低于 2 GB 时，工具会等到有空间再继续，除非给了 `--disk-stop` 或 `--disk-stop-remove`。
- **续跑：**中途停下（磁盘满、被关掉、崩溃）的运行会保留工作文件夹。再运行同一条命令，就从停下的地方继续，结果和一次跑完相同。参数、某个母带、`--core` 给的文件或工具本身变了，就从头开始；改了 `--small-disk` 也从头开始。标签选项、`--mem`、`--timing`、`--force-disk`、`--disk-stop`、`--disk-stop-remove` 不算在内：续跑前可以改。
- **实测的用时、内存和磁盘**（6、10、30 分钟，2 到 12 个母带）见 [SPEC.zh.md](SPEC.zh.md) 第 11.5 节。这些测试中峰值内存都低于 2 GB 的预算。工作文件夹随长度以及母带的数量和采样率增长：30 分钟、12 个母带要 27.8 GB，用 `--small-disk` 是 15.0 GB。

## 标签和图片

`laes enc` 和 `laes tag` 用同样的选项。一个都不给时不写标签，文件和不支持标签的版本做出的逐字节相同。

| 选项 | 含义 |
|---|---|
| `--tag=KEY=VALUE` | 一个标签；KEY 是 Vorbis comment 的字段名，例如 `TITLE`、`ARTIST`、`ALBUM`、`ALBUMARTIST`、`DATE`、`GENRE`、`TRACKNUMBER`、`DISCNUMBER`、`COMPOSER`、`COPYRIGHT`、`COMMENT`、`ISRC`。同一个 KEY 可以给多次。 |
| `--cover=<图片>` | 封面 |
| `--picture=<类型>:<图片>` | 其他图片，张数不限：4 封底、5 内页、6 碟面、7 主要艺人、8 艺人……（ID3v2 和 FLAC 共用的图片类型，0 到 20） |

- **图片：**JPEG 或 PNG，逐字节原样保存，从不重新压缩。不设尺寸下限；一张最大约 16 MB（FLAC 一个元数据块的上限），全部图片合计最大 256 MB。图片很大时，有些手机播放器打开文件会变慢。
- **文字：**UTF-8，什么语言都可以。KEY 本身是 ASCII。
- **标签放在哪里：**一套标签描述核心母带和所有母带。

| 形式 | 标签 | 图片 | 位置 |
|---|---|---|---|
| FLAC | 一个 `VORBIS_COMMENT` 块 | 每张图一个 `PICTURE` 块 | STREAMINFO 之后、`LAE2` 块之前 |
| WAV | 一个 `LIST`/`INFO` 块和一个 `id3 ` 块（ID3v2.4） | `id3 ` 块里的 `APIC` 帧 | `fmt ` 之后、`data` 之前 |

- WAV 的 `INFO` 只认识部分字段：`TITLE` `INAM`、`ARTIST` `IART`、`ALBUM` `IPRD`、`DATE` `ICRD`、`COPYRIGHT` `ICOP`、`COMMENT` `ICMT`、`GENRE` `IGNR`、`TRACKNUMBER` `ITRK`。所有字段都写进 `id3 `；没有对应 ID3 帧的字段写进 `TXXX`。
- 按系统代码页读 WAV `INFO` 的老软件，非 ASCII 文字会显示错。`id3 ` 永远是 UTF-8，两种都读的播放器应当优先用它。
- 工具在标签里写自己的名字 `laes (LAE Lossless)`，不带版本号，所以同样的输入永远得到同样的文件。
- 标签从不参与校验：每个母带的 SHA-256 只覆盖样点，标签也从不决定播哪个版本。

示例：

```
laes enc Piece_LAES.flac --derive-core Piece_352k_32.wav Piece_176k_24.wav "--tag=TITLE=Piece" "--tag=ARTIST=Name" "--tag=DATE=2026" "--tag=COPYRIGHT=(c) 2026 Name. All rights reserved." --cover=cover.jpg --picture=4:back.jpg
```

## 修改标签：`laes tag`

```
laes tag <输入> <输出> [--tag=KEY=VALUE ...] [--cover=<图片>] [--picture=<类型>:<图片> ...]
```

- 写一个**新**文件，标签和图片正好是给出的这些；不给标签选项时，新文件完全没有标签。
- 旧标签去掉；音频字节、载荷以及其他所有块逐字节复制。
- 结果和用这些标签重新编码母带得到的文件逐字节相同。
- 写完后完整解码新文件，每个母带都和存的 SHA-256 核对，音频字节和载荷还要和旧文件比较。有任何不同就删掉新文件。
- 旧文件从不改动。`<输出>` 已经存在时什么都不做。
- FLAC 形式和 WAV 形式（包括 RF64）都可以。LAEY 文件（`LAE1`）用 `lae_enc tag` 改标签。

## 查看结构：`laes parts`

```
laes parts <文件>
```

列出 FLAC 的元数据块或 RIFF 的块及其大小，打印音频字节（FLAC 的帧，或 WAV 的 `data`）和载荷各自的 SHA-256，以及播放器读到的标签。两个文件的这两个 SHA-256 都相同，就说明音频和母带完全相同，不管标签是否不同。

## 解码：`laes dec`

```
laes dec <输入> <输出文件夹> [--levels]
laes dec <输入> --list
laes dec <输入> <输出文件夹> --pick=<n>[,<n> ...] | --pick=<采样率>:<位数>
```

- 不加选项时写出核心母带（`core.wav`）和每个母带（`master_<k>_<采样率>_<位数>.wav`）。
- `--levels` 另外写出每个母带的每个采样率级别（`master_<k>_level_<采样率>_<位数>.wav`）。采样率级别就是母带在一半、四分之一或八分之一采样率下的版本，无损。
- `--list` 列出所有选择并编号；`--pick` 只写选中的，可以按编号，也可以按采样率和位数选。
- **派生 16 位母带：**没有自己的 16 位选择的采样率，都提供一个 16 位版本（`derived16_<采样率>_from_<名称>.wav`）。它由该采样率下字长最长的选择按固定的 TPDF 规则（[SPEC.zh.md](SPEC.zh.md) 第 10.1 节）得到，每个解码器都能逐位还原。相对这条规则它是无损的；相对所取自的那个选择不是无损的：那个选择用 TPDF 抖动重新量化到 16 位，无法从它还原。它是派生母带，不是原始母带；只有被选中时才写出。
- SHA-256 不符的母带不提供，它的采样率级别也不提供；核心母带和其他母带不受影响。
- 超过 16 位的输出写成 `WAVE_FORMAT_EXTENSIBLE`。

## 校验：`laes verify`

```
laes verify <输入> <母带.wav> [<母带.wav> ...]
```

解码文件里的每个母带，和给出的文件逐位比较，顺序不限。每个文件打印 `IDENTICAL`（相同）或 `DIFFERENT`（不同）；没有给文件的母带打印 `not compared`（未比较）。

## 时间位置：`laes align`

```
laes align <cd.wav> <母带.wav> [--lae-enc=<路径>] [--accept]
```

用于单独制作的核心母带，例如已发行的 CD：用 LAE 有损编码器（`lae_enc`，在 `laes` 旁边找，或用 `--lae-enc` 指定）的自动搜索，测量母带在 CD 时间轴上的时间位置 T，包括小数部分。对齐误差在 −60 dB 以内直接通过；否则显示误差，由你决定。它不写任何文件，只打印 `use <母带>@<T>` 供 `laes enc` 使用。整张专辑按轨切分用 LAE 有损的规划器（`lae_enc plan … laes`）。

## 测试辅助命令

| 命令 | 作用 |
|---|---|
| `laes selftest` | 用合成信号跑回归测试；退出码 0 = 全部通过 |
| `laes excerpt <in.wav> <out.wav> <开始秒> <秒数> <位数> [补秒]` | 截取片段；16 = TPDF 降到 16 位，24 = 原样，32 = 低字节随机 |
| `laes reduce <in.wav> <out.wav> <位数>` | 用 TPDF 抖动缩短字长，固定种子 |
| `laes mkcd <母带.wav> <cd.wav> <tau> [增益] [噪声]` | 制作测试用的"CD"：16 位，母带的基础级别平移 tau |

## 文字、文件名和退出码

- **文字：**在 Windows 上，工具内部的参数、文件名、标签和屏幕输出全部用 UTF-8。不管系统代码页是什么，在 cmd、PowerShell 和脚本里都能用任何语言的标签和路径。其他系统上参数按原样处理（通常就是 UTF-8）。
- **退出码：**0 成功；1 参数错误；2 工作失败（文件读写失败、检查不通过、磁盘满）。退出码为 2 时，`laes enc` 和 `laes tag` 什么都没写。
- **提示信息**是英文；磁盘和续跑相关的提示另有一行中文。
