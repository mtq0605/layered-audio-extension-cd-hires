# LAE Java 库

[English](README.md)（以英文版为准）

本库是 LAE 编码器和解码器的纯 Java 实现，供 Android 及其他 Java 平台使用。本库不含本地代码。其输出与同一提交哈希的 C++ 参考工具的输出相同：解码得到的采样相同，编码得到的文件逐字节相同。

## 内容

| 包 | 内容 |
|---|---|
| `com.lae.core` | 公共工具：确定性数学函数（`LMath`，即 `src/lae_math.h` 的 Java 版）、半带滤波器、对齐哈希、小端序读写 |
| `com.lae.laey` | LAE Lossy（LAEY）：解码器、编码器（含独立 CD 对齐以及选项 `noise=` 和 `g`，即噪声标记及其第二遍编码，见 [docs/NOISE.md](../docs/NOISE.zh.md)）、命令行工具 `LaeEncTool` |
| `com.lae.laes` | LAE Lossless（LAES）：解码器、播放器前端（`Play`）、编码器、标签和图片、命令行工具 `LaesTool` |

本库不包含 CD-Extra 封装（`lae_cdx`）、光盘镜像和整盘规划器（`lae_enc plan`）。

## 运行要求

- Java 8（语言级别和类库）。
- Android：API 级别 24（Android 7.0）或更高。本库不使用 API 级别 24 之后新增的任何 API。
- 无第三方依赖。

## 构建

```bash
javac -encoding UTF-8 --release 8 -d out/classes $(find src -name '*.java')
```

如需检查是否用到了 Android 7.0 之后新增的 API，可使用 `-source 8 -target 8 -bootclasspath <Android SDK>/platforms/android-24/android.jar` 再次编译源代码。

## 命令行用法

两个工具接受与 C++ 工具相同的参数，并输出相同的信息，例外情况在示例之后给出：

```bash
java -cp out/classes com.lae.laes.LaesTool enc out.flac --derive-core master.wav
```

```bash
java -cp out/classes com.lae.laes.LaesTool dec in.flac outdir
```

```bash
java -cp out/classes com.lae.laey.LaeEncTool master.wav out.wav
```

`LaesTool` 支持 `enc`、`dec` 和 `verify`。`LaeEncTool` 支持 `lae_enc` 的编码选项；不包含整盘规划器（`lae_enc plan`）。完整参数列表见 `LaesTool.java` 和 `LaeEncTool.java` 的文件头。对于 `lae_enc` 会在控制台提问的情况（`bits=24`、`cdaccept`、`cdedge=`），Java 工具不提问，而是以 `lae_enc` 在无控制台运行时输出的信息停止，此时必须在命令行中给出该选项。

## 与 C++ 工具的一致性

Java 代码移植自 `src/` 中的 C++ 参考实现，移植不改动该实现。移植的每一步均通过以下方式检验：用两种实现对相同输入进行编码和解码，并逐字节比较结果。可能影响输出的浮点运算步骤使用 `LMath`，它精确复现 C++ 工具的 `lmath::`。

LAEY 解码接口（`LaeyDecoder`）有两个只在 Java 中提供、面向手机的调用，都不改变输出：

- `lae_open_file(path, cdCacheBytes)`：CD 层按需从文件读取，不整个放进内存。
- `lae_dup()`：在已经打开的文件上再开一个曲目。CD 层和 LAE 块共用，不重新读取，也不重新计算哈希；已解出的跳转检查点一并沿用。它的路径、扩展包和解码状态各自独立，因此播放器可以在第二个线程中准备新路径，旧路径同时继续播放。

Java 移植有两处行为不同：

- 播放器前端 `Play` 只对母带不超过 16 个的 LAES 文件提供各母带（`Play.File.maxMasters`，是常量）；母带更多的文件播放核心母带，并报告原因。设这个上限，是因为较长的参照链所需内存超过手机的内存（在 PC 上实测：参照链中每个母带约需 1 MB 堆内存）。格式允许 255 个母带；Java 版 LAES 编码器在开始之前就拒绝超过 16 个母带的任务，母带更多的文件用 PC 上的工具（`laes enc`）制作。
- 有多个母带符合 `--pick=<rate>:<bits>` 或 `--pick=name:<text>` 时，`LaesTool` 列出候选、不写出任何内容，与 C++ 工具不在控制台中运行时相同；它没有勾选列表。

## 许可

许可与 C++ 代码相同。解码器和公共工具采用 Apache-2.0 许可。编码器文件采用 GPL-3.0-or-later 许可或商业许可（见 [COMMERCIAL.md](../COMMERCIAL.zh.md)）。播放器前端 `Play` 属于解码器文件（Apache-2.0）；标签和图片代码（`LaesTags`、`LaeRetag`）以及命令行工具（`LaesTool`、`LaeEncTool`）属于编码器文件。每个文件在其 SPDX 文件头中写明其许可。`com.lae.laey` 中只有编码器使用的三个文件也属于编码器文件（2026-11-15 的决定）：`WavIo`（编码器使用的 WAV 代码）、`LaeDec`（编码器对自己的输出运行的解码器，带有给编码器的两项导出）和 `NoiseMarks`（编码器读取标定文件的代码）。所有 Apache-2.0 文件，包括解码器 `LaeyDecoder`、`LaeDecoder`、`LaesDecoder` 和播放器前端 `Play`，不需要任何编码器文件即可编译（2026-11-15 用 javac 17 检查）。
