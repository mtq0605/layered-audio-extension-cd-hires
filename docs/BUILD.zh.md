# 构建 LAE

[English](BUILD.md)（以英文版为准）

<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC-BY-4.0
-->

## 1. 编译器与选项

全部代码均为标准 C++17，不使用任何第三方库。

- 编码器由 `src/lae_enc.cpp` 构建。
- 解码器由 `src/lae_dec.cpp` 构建。
- CD-Extra 工具 `lae_cdx` 由 `src/lae_cdx.cpp` 构建。
- LAE Lossless 工具 `laes`（编码器、解码器、对齐及自检）由 `src/laes_tool.cpp` 构建。LAES 以 IEEE-754 双精度、按源代码顺序计算其参考滤波器系数（[spec/lossless/SPEC.zh.md](../spec/lossless/SPEC.zh.md)，第 2.2 节）。构建 `laes` 和 `lae_cdx` 时须使用 `/fp:strict`（MSVC）或 `-ffp-contract=off`（GCC / Clang）。
- 解码器库 `liblae` 由 `src/lae_api.cpp` 构建。
- foobar2000 组件使用 `plugins/foo_input_lae/build_fb2k.cmd` 构建（[plugins/foo_input_lae/README.zh.md](../plugins/foo_input_lae/README.zh.md)）。
- 编码器使用 `std::filesystem`。在 Windows 以外的系统上，编码器还使用 `setenv`/`unsetenv`。
- 各程序的源文件列于 [README](../README.zh.md) 第 14.1 节。
- 在 Windows 上，系统代码页以外的文件名和标签需要 Windows 10 1803 或更新的版本（各工具使用 UTF-8 区域设置，`src/win_utf8.h`）；在更旧的 Windows 上，工具打印一条警告，按系统代码页工作。

| 平台 | 编译器 | 选项 |
|---|---|---|
| Windows x64 | MSVC 2022 | `cl /O2 /EHsc /std:c++17 /utf-8 /D_USE_MATH_DEFINES src\lae_dec.cpp`（编码器：另加 `/bigobj /fp:strict`） |
| Windows x86（32 位） | MSVC 2022 | 同上；SSE2 为 MSVC 32 位的默认设置 |
| Linux / macOS x86-64 | GCC / Clang | `-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread` |
| Linux x86（32 位） | GCC / Clang | 另加 `-msse2 -mfpmath=sse`（**必需**） |
| ARM（32/64 位） | GCC / Clang | `-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread` |

**切勿使用** `-ffast-math`、`/fp:fast` 或任何其他允许融合乘加收缩的选项。

**MSVC 必须始终带 `/utf-8`**。源文件是不带字节顺序标记的 UTF-8，其中一些在字符串字面量里含有中文（例如 `lae_cdx` 写到光盘上的 `README.TXT` 和 `BURN.TXT`，以及插件的中文文字）。不带 `/utf-8` 时，MSVC 按系统代码页读源文件；在中文 Windows 上是 GBK（代码页 936），文字会因此损坏，但音频不受影响：

- 在普通字面量（`"..."`）里，按 GBK 配不成对的字节被替换成 `?`，一个字就被截断了；
- 在 `u8"..."` 或 `L"..."` 字面量里，文字被转换成别的字，不出现 `?`；
- 警告 C4819 只在遇到 GBK 表示不了的字符时出现，所以构建可能已经损坏却没有任何警告。

每个 MSVC 调用都要带 `/utf-8`，包括编译目前纯 ASCII 的文件时（也可以设环境变量 `CL=/utf-8`）。GCC 和 Clang 默认按 UTF-8 读取；`javac` 调用时带 `-encoding UTF-8`。

**发布构建之后的检查**。打包程序之前：

1. 任何编译记录中出现警告 C4819 都按错误处理（或用 `/we4819` 编译）。
2. 检查每个程序（包括插件安装包里的 DLL）中是否有损坏的中文：最后一个字节变成 `?` 的汉字，以及源码中已知中文短语的 `u8` / `L` 损坏形式。中文版插件必须含有形式正确的中文文字。
3. 检查程序写出的每个文本文件（例如光盘的 `README.TXT` 和 `BURN.TXT`，也包括光盘映像和 `DATA.ISO` 里的）是合法的 UTF-8，并且没有上述损坏。

仅当以下两个条件同时成立时，输出在所有平台上才逐位一致：

- 解码器的逆 MDCT 以严格的 IEEE-754 双精度、按源代码顺序计算；
- 所有常量均取自 `src/mdct_tables.h` 中的精确十六进制字面量。

**不支持 x87 构建**。解码器的启动自检会拒绝此类构建（第 2 节）。

**编码器在所有平台上同样逐位一致（2026-11-03**）。凡决定其输出的部分（码长、检测器窗口与阈值、滤波器系数、FFT 以及对齐）均使用 `src/lae_math.h` 中的函数（`log2`、`sinpi`、`cospi`、`exp2`、`log10`），而不使用 C 库中的对应函数。这些函数仅按固定顺序使用 IEEE-754 运算 `+ − × ÷`、`sqrt`、`floor`、`frexp` 和 `ldexp`，与 LAES 在 `src/laes_math.h` 中的做法相同。频率相等的音调按其被发现的顺序合并（`std::stable_sort`）。编码器应使用上表中的标志构建（即解码器的标志，在 MSVC 上另加 `/fp:strict`；在 GCC / Clang 上使用 `-ffp-contract=off`；在 32 位 x86 上使用 SSE2）：此时 x64 与 x86 构建写出逐字节一致的文件，`tests/check_encoder_*` 将任何不一致的文件判为失败。`tools/lae_math_check.cpp` 将这些函数与 C 库进行对比测量（`log2` 至多相差 3 个末位单位），并输出一个在所有平台上必须相同的校验和。在 2026-11-03 之前，C 库的 `log2` 在不同平台之间可能在最后一位上存在差异，从而改变两个代价相等的划分之间的选择；解码所得的音频则相同。

第 3 节列出已经验证的构建。其他构建，包括 32 位 ARM 及 ARM 上的 macOS，尚未验证。

示例（Linux，解码器与编码器）：

```sh
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_dec src/lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_enc src/lae_enc.cpp
g++ -O2 -std=c++17 -ffp-contract=off -pthread -o laes src/laes_tool.cpp && ./laes selftest
```

示例（Windows，`lae_cdx` 与 `laes`；LAES 参考滤波器系数要求使用 `/fp:strict`）：

```bat
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /bigobj src\lae_cdx.cpp
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_CRT_SECURE_NO_WARNINGS src\laes_tool.cpp /Fe:laes.exe
laes selftest
```

示例（Windows，统一解码器 `lae_udec`，它把 `liblae` 和两种格式的解码器静态链接在一起）：

```bat
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /DLAE_STATIC /bigobj src\lae_udec.cpp src\lae_any.cpp src\lae_api.cpp src\laes_api.cpp /Fe:lae_udec.exe /link /MANIFEST:EMBED /MANIFESTINPUT:src\win_longpath.manifest
```

`liblae`（LAE Lossy 解码器，`src/lae_api.cpp`）以 `/fp:precise` 编译，满足 [spec/lossy/SPEC.zh.md](../spec/lossy/SPEC.zh.md) 第 2.2 节的浮点规则：不进行收缩，因为构建脚本（MSVC 2022）不带 `/arch:AVX2` 或更高的选项，编译器没有可用的融合乘加指令。不要加这样的选项。插件中的 LAE Lossless 解码器（`src/laes_api.cpp`）以 `/fp:strict` 编译。经验证的标志见各插件的构建脚本，例如 `plugins/foo_input_lae/build_fb2k.cmd`。

### 辅助程序

仓库中还包含以下程序。各文件的头部注释说明了其用法。

- `tools/selftest_ref.cpp` 输出某一构建的自检指纹（第 2 节）。
- `tools/gen_halfband.cpp` 和 `tools/gen_mdct_tables.cpp` 分别生成 `src/halfband.h` 和 `src/mdct_tables.h`。仓库里的 `src/mdct_tables.h` 是规范表：生成器把结果写到当前文件夹，只用来对照。用 x64 MSVC 编出的生成器得出的表和它完全相同；按 x86（`/arch:SSE2`）编出的有 75 个值最后一位不同（2026-11-14 实测），别的编译器或 C 库也可能不同，因为生成器用的是 C 库的 `sin` 和 `cos`。重新生成的表一律不要提交：所有解码器必须用同一组常数。这两个生成器（以及 `gen_noise_test`）不会悄悄替换已有的输出文件：在控制台上问两次，两次都回答 yes 才替换；不在控制台时拒绝，除非给了 `--overwrite`（README 第 14.11 节）。
- `tools/test_mdct_tb.cpp` 是 `src/mdct_tb.h` 的测试程序。
- `tools/lae_math_check.cpp` 将编码器在 `src/lae_math.h` 中的函数与 C 库进行对比检查，并输出其平台校验和。
- `src/gen_suite.cpp` 生成合成测试信号（可以只生成一个类别）。
- `src/cd_variant.cpp` 生成定义精确的 CD 或母带测试变体（平移、补静音、截去开头、增益、极性、左右互换、淡出等），用于对齐和扩展包测试（`tests/check_packs.sh`）。
- `tools/lae_abx/lae_abx.cpp` 是独立运行的 ABX 听音测试（Windows x64，需要 Windows 10 1703 版或更新版本：它使用该版本的按显示器 DPI 函数），需要旁边有 `lae_udec.exe`。编译（x64）：`cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE tools\lae_abx\lae_abx.cpp user32.lib gdi32.lib comctl32.lib ole32.lib shell32.lib mfplat.lib mfreadwrite.lib mfuuid.lib winmm.lib comdlg32.lib`。准备好的音源连同 `lae_abx` 和旁边 `lae_udec.exe` 的标识一起保存在 `%TEMP%\lae_abx_cache`，别的版本做的副本会重新准备；"Clear kept copies"按钮清除这些副本。每一对的日志从开始就写，每答一题记一行；整轮总结写出每一对的测试采样率。`lae_abx --prepare <pair> <rate> <folder> [--overwrite]` 不加 `--overwrite` 就不替换已有的输出。
- `src/cut_wav.cpp` 和 `src/cmp_wav.cpp` 是 WAV 实用程序。`src/cmp_wav.cpp` 计算 README 第 12.1 节所定义的误差度量。

## 2. 启动自检

每次启动时，解码器将一个固定的数据模式送入逆 MDCT，并对结果计算哈希。

- **哈希**。该哈希为采用非标准偏移基的 64 位 FNV-1a 型哈希（[SPEC.zh.md](../spec/lossy/SPEC.zh.md)，第 13 节）。
- **期望值**。该哈希必须等于 `0x729b547fbdbfe203`（`src/mdct_tb.h` 中的 `SELFTEST_REF`）。
- **不一致时**。若哈希不同，则表明该构建改变了浮点行为。此时解码器拒绝运行（退出码 4），而不产生与其他平台不同的输出。

`tools/selftest_ref.cpp` 输出某一构建的哈希。

## 3. 已验证的构建

| 构建 | 解码器：16 个向量 × 4 条路径 | 编码器：16 个向量 |
|---|---|---|
| MSVC 2022 x64 `/O2` 与 `/Od`，x86 SSE2（Windows） | 一致 | 参考文件（各向量即由这些构建写出；x64 与 x86 写出相同的文件，第 1 节） |
| GCC 13 x86-64 `-O2` 与 `-O0`（Linux） | 一致 | 与参考文件逐字节一致 |
| Clang 18 x86-64 `-O2`（Linux） | 一致 | 未检查 |
| GCC 13 x86 32 位 SSE2（Linux） | 一致 | 未检查 |
| GCC 13 ARM64（aarch64）`-O2` 与 `-O0`（Linux） | 一致 | `-O2`：与参考文件逐字节一致 |
| Clang 18 ARM64（aarch64）`-O2`（Linux） | 一致 | 未检查 |

在 Linux x86 上，256 个解码器结果中有 256 个与参考一致；在 Linux ARM64 上，192 个中有 192 个一致。

`liblae` 和 foobar2000 组件仅在 Windows 上使用 MSVC 2022 构建和测试过（README，第 13.3 和 13.7 节）。`laes` 和 `lae_cdx` 不在表中：在 Linux 上，CI 在 x86-64 和 ARM64 上用 g++ 和 clang++ 构建这两个程序，并运行 `laes selftest` 和 `lae_cdx selftest`（`tests/check_laes_linux.sh`）；它们的其他命令 CI 在该平台上不运行。2026-11-14 在 Linux x64（WSL 下的 g++ 13.3）上运行过一次 `laes` 的 `dec`、`verify`、`extract` 和 `add` 命令，以及 `lae_cdx` 的读取器（`read` 和 `repack`，WAV 和 FLAC 抓轨），输出与 Windows 构建逐字节相同；`laes enc` 和 `laes align` 没有在该平台上检查过（[spec/lossless/LAES_TOOL.md](../spec/lossless/LAES_TOOL.zh.md) 的"编译"一节）。

Linux 上的检查由五个脚本执行：

- `tests/check_vectors_linux.sh` 检查解码器；
- `tests/check_encoder_linux.sh` 检查编码器；
- `tests/check_laes_linux.sh` 构建 `laes` 和 `lae_cdx` 并运行它们的自检；
- `tests/check_noise_linux.sh` 检查噪声扫描；
- `tests/check_tags_linux.sh` 检查标签和图片。

这些脚本输出比较结果的摘要。`.github/workflows/` 中的一个 GitHub Actions 工作流运行这些脚本。

**手动运行的检查（2026-11-15**）。下面几项检查工作流不运行，发布之前在本机手动运行；下次修改工作流时再把它们加进去：

- `tests/check_packs.sh <bin folder> <older lae_dec> <scratch folder>`：扩展包（Windows 上用 Git Bash，或 Linux）；
- `tests/check_flacform.sh <lae_enc> <lae_dec> <master.wav> <scratch folder>`：FLAC 形式；
- `tools/lae_math_check`：编码器在 `src/lae_math.h` 中的函数与 C 库对照，并给出一个在所有平台上都必须相同的校验和；
- `laes` 的 32 位（x86）构建及其 `laes selftest`。

一致性测试向量及其哈希列于 [tests/vectors/MANIFEST.md](../tests/vectors/MANIFEST.md)。

## 4. 线程

- **编码器**。编码器的划分搜索按根块并行执行。
  - 默认情况下，线程数为系统报告的硬件线程（逻辑处理器）数（`std::thread::hardware_concurrency`）。
  - 测试开关 `PARTTHREADS` 可显式设定线程数；和 `lae_enc` 的每个测试开关（`LAEY_TEST_PIECE`、`LAE_TESTMAXFRAMES`、`LAE_TESTFAIL`、`LAE_BLOCKUNITS`、`LAE_TESTSEQUENTIAL`、`PARTTHREADS`）一样，只有设了 `LAE_TEST_MODE=1` 才起作用，这时每次运行打印一行，列出起作用的开关。`TIMING`、`KEEPTMP` 和 `TIERSTAT` 不改变输出的任何字节，不设也起作用（README 第 14.11 节）。
  - 线程数不影响结果：1 个线程与 16 个线程产生逐字节一致的码流。
  - 自 2026-10-29 起，路径分离的各滤波器也分配到多个线程上执行，且一对中的两个层（位深层 L 与 R、频率层 M 与 S，以及泄漏补偿后的 L 与 R）可以同时编码。每个输出值均以相同的运算、按相同的顺序计算，因此输出逐字节一致。
  - 内存规划：启动时，编码器读取一次物理内存大小，并根据音轨长度决定每对中的两个层是同时运行还是先后运行（`memory plan: …`）。仅当为 64 位构建、`PARTTHREADS` 不为 1，且约 2 × （每个层样本 1 字节 + 128 MB）同时不超过内存上限与可用内存时，两个层才同时运行。内存上限通过 `mem=<GB>` 设定；默认值为 min(1/2 × 物理内存, max(1/4 × 物理内存, 物理内存 − 28 GB))：在 32 GB 的机器上为 8 GB，在 48 GB 上为 20 GB，在 64 GB 上为 32 GB。块长度取不超过该上限的最大值，至多 4 个单位；一个单位为 lcm(seg, 8) 个 MDCT 帧，其中一个基底段 seg 约为一秒的帧数，因此在 352.8 kHz 下 4 个单位约为 32 s。`LAE_BLOCKUNITS` 可手动设定单位数（测试开关，只在设了 `LAE_TEST_MODE=1` 时起作用；下文的 512 s 块即以此方式设定）。在一个 10.16 分钟的 352.8 kHz 测试母带（未公开）上的测量结果：32 s 块，767 s / 471 MB；24 s 块（`mem=1`），776 s / 408 MB；512 s 块，803 s / 6,921 MB；所有输出均逐字节一致。在 Linux 上，编码器读取 `/proc/meminfo`（MemAvailable）；在无法读取内存大小的情况下，各层先后运行。
  - `PARTTHREADS=1` 使整个编码器以单线程运行。
  - 任一线程中的错误（内存不足、写入失败）均传递给主线程，并按主线程中的错误同样处理：不写出任何输出，并删除临时文件夹（给出 `resume` 时保留该文件夹，供下次运行使用；README 第 14.2 节）。
- **解码器**。解码器为单线程。

## 5. 速度与内存

以下数值为开发期间（2026-10-16）在 Windows x64 上使用一个 352.8 kHz、40 s 的立体声文件测得。

| | 时间 | 峰值内存 |
|---|---|---|
| 解码（24 位路径，流式） | 2.8 s（约为实时的 14×） | 38 MB |
| 编码（模式 3，16 个线程；2026-10-16 的编码器） | 113 s（约为实时的 0.35×） | 1.35 GB |

2026-10-29 的编码器（Windows x64，32 个线程，352.8 kHz 立体声，60 s，模式 3，s1；输出与 2026-10-24 编码器的输出逐字节一致）：

| 编码器 | 时间 | 峰值内存 |
|---|---|---|
| 2026-10-24 | 177 s | 1.83 GB |
| 2026-10-29，各层先后运行 | 59 s | 1.06 GB |
| 2026-10-29，一对中的两个层同时运行 | 43 s | 1.49 GB |

2026-10-29 的分块编码器（同一机器，模式 3，s1；输出与整轨编码器的输出逐字节一致）：

| 母带 | 时间 | 峰值内存 |
|---|---|---|
| 352.8 kHz 立体声，60 s | 127 s（机器同时承受其他运行的负载） | 482 MB |
| 352.8 kHz 32 位立体声，10.16 min（bits=24） | 492 s | 475 MB（整轨编码器：各层先后运行时为 580 s，12,039 MB） |
| 352.8 kHz 立体声，13 min（超出此前 Windows 在 352.8 kHz 下约 12.7 分钟的限制） | 961 s | 733 MB（使用 2026-10-29 的构建测得，当时 `LAE ` 块写出时尚会额外保留码流的第二份副本） |

对于上述整轨编码器，时间和内存随长度线性增长（在 10、20、40 和 60 s 下测得）。节省的时间大部分来自划分搜索：该搜索此前为每个矩形分配一个切分列表；在线程较多时，这些堆分配相互等待。内存的节省来自三项改动：只保留一个重建平面，而不是两份相同的副本；每个倍频程段只保存其自身的频点；信号在不再需要时立即释放。

**解码器**。解码器为流式，其内存用量不随音轨长度增长，但输入文件除外，目前输入文件被整体读入。解码器：

- 每次解码一行根块（8 帧）；
- 逐帧对逆 MDCT 进行重叠相加；
- 通过短滑动窗口拉动每一个重采样级。

**编码器（分块，2026-10-29**）。模式比例及其他全轨决策在整个音轨上测量，但音轨按块读取和编码：

- 母带逐段读取（WAV、RF64、Sony Wave64）；
- CD 层、路径分离和层编码器以由完整基底段和完整根块行构成的块（约 32 s）为单位工作，每个块在读取时附带其滤波器所需的边距；每个值均以与此前相同的运算、按相同的顺序计算，因此输出逐字节一致；
- 各遍扫描累积全轨摘要（基底、音调与瞬态事件、平均能量、码长统计、份额），根据这些摘要制定计划（标记、lambda 值），然后按该计划对每个块进行编码；
- 区间编码器及其自检每次处理一行根块，输出文件逐段写出并逐段读回。

不使用 `cd=` 时，内存用量不随长度增长，但每个单元的记录（每个单元 3 字节）除外；关于 `cd=`，见下文。大型中间数据写入工作文件夹：对一个 10.16 分钟的 352.8 kHz 32 位母带（一个未公开的测试母带；bits=24，模式 3，s1；8 个线程下为 823 s，峰值 465 MB），测得的最大值为 13,185 MB，约为每个母带帧 61 字节。自 2026-11-05 起工作文件夹变小（同样长度和采样率的母带为 8.0 GB；README 第 14.2 节），运行前的可用空间检查按每个母带帧 45 字节估算（使用 `cd=` 时为 55 字节，使用 `g` 时再加 12 字节）。使用 `cd=` 时，母带同样通过文件窗口读取，在 CD 时间线上的放置（在移位后的母带前后添加的数字静音、整样本移位本身以及对该静音的裁切）由文件写入文件；但对齐搜索本身以 CD 采样率在整个音轨上进行相关运算（10 分钟约需 2.4 GB）。选项 `resume` 从中断运行的最后一个已完成步骤继续（使用 `cd=` 时，从对齐之后继续，对齐将重新执行）。

**解码器输入**。解码器就地读取其输入文件中的 CD 音频（一份副本，每个 CD 样本 2 字节）。

**LAE Lossless（`laes`，分块，2026-10-30）。**`laes enc`、`dec`、`verify` 和 `align` 以基础档位的分段为单位工作（一次处理一个分段的全部采样率档位）；输出与 2026-10-29 的整轨 `laes` 的输出逐字节一致。每个存储候选均写入一个工作文件夹（`<output>.laes.tmp`，结束时删除；`KEEPTMP=1` 可保留该文件夹），且仅保留最小者；WAV 和 FLAC 文件逐段写出并逐段读回。内存上限通过 `--mem=<GB>` 设定；默认值为 2 GB，若 min(1/2 × 物理内存, max(1/4 × 物理内存, 物理内存 − 28 GB)) 更小则取该值（自 2026-11-04 起）。以下测量于 2026-10-30 进行，当时的默认值为 min(1/2 × 物理内存, max(1/4 × 物理内存, 物理内存 − 28 GB))，没有 2 GB 的上限。大于 4 GiB 的 WAV 输出以 RF64 形式写出（[spec/lossless/SPEC.zh.md](../spec/lossless/SPEC.zh.md)，第 7 节）。

在 Windows x64（32 个线程，32 GB）上测得：

| 内容 | 步骤 | 时间 | 峰值内存 |
|---|---|---|---|
| 60 s，四个母带（352.8 kHz 下 32 / 24 / 16 位，176.4 kHz 下 24 位） | 编码，整轨 `laes` | 256 s | 5,743 MB |
| | 编码，分块 | 236 s | 1,017 MB（`--mem=1`：373 MB） |
| | 解码，整轨 / 分块 | | 4,704 MB / 773 MB |
| 10:09（未公开的测试母带），核心母带：一张独立 CD，以及相同的四个母带 | 编码，FLAC 形式 | 1,661 s | 2,860 MB |
| | 编码，WAV 形式 | 1,166 s | 2,859 MB |
| | 解码并校验 | 约 175 s | 2,239 MB |
| | 编码，FLAC 形式，`--mem=20`（8 个线程） | 3,585 s | 6,553 MB |
| | 编码，FLAC 形式，`--mem=2`（8 个线程） | 3,655 s | 1,034 MB |

同一形式的所有编码行产生相同的文件。**磁盘空间**：对于 10 分钟、四个母带的情形，工作文件夹达到 38.6 GB（实测；`--mem=20` 与 `--mem=2` 相同），对于这四个母带约为每分钟音乐 3.8 GB；该文件夹在结束时删除。应在输出文件所在的驱动器上保留这么多可用空间，另加输出文件本身所需的空间。
