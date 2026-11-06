# LAE Java 库

[English](README.md)（以英文版为准）

LAE 编码器和解码器的纯 Java 实现，供 Android 及其他 Java 平台使用，不含本地代码。其结果与同一提交号的 C++ 参考工具相同：解码出相同的采样，编码出逐字节相同的文件。

## 内容

| 包 | 内容 |
|---|---|
| `com.lae.core` | 公共工具：确定性数学函数（`LMath`，即 `src/lae_math.h` 的 Java 版）、半带滤波器、对齐哈希、小端序读写 |
| `com.lae.laey` | LAE Lossy（LAEY）：解码器、编码器（含独立 CD 对齐、`noise=` 和 `g`）、命令行工具 `LaeEncTool` |
| `com.lae.laes` | LAE Lossless（LAES）：解码器、播放器前端（`Play`）、编码器、标签和图片、命令行工具 `LaesTool` |

不包含：CD-Extra 封装（`lae_cdx`）、光盘镜像、整盘规划器（`lae_enc plan`）。

## 运行要求

- Java 8 语言级别和类库。
- Android：API 24（Android 7.0）或更高。本库不使用 API 24 之后新增的接口。
- 无第三方依赖。

## 构建

```bash
javac -encoding UTF-8 --release 8 -d out/classes $(find src -name '*.java')
```

如需检查是否用到了 Android 7.0 之后新增的接口，可用 `-source 8 -target 8 -bootclasspath <Android SDK>/platforms/android-24/android.jar` 再编译一次。

## 命令行用法

两个工具的参数和输出信息与 C++ 工具相同：

```bash
java -cp out/classes com.lae.laes.LaesTool enc out.flac --derive-core master.wav
```

```bash
java -cp out/classes com.lae.laes.LaesTool dec in.flac outdir
```

```bash
java -cp out/classes com.lae.laey.LaeEncTool master.wav out.wav
```

`LaesTool` 支持 `enc`、`dec` 和 `verify`。`LaeEncTool` 支持 `lae_enc` 除 `plan` 以外的编码选项。完整参数见 `LaesTool.java` 和 `LaeEncTool.java` 的文件头注释。`lae_enc` 会在控制台提问的情况（`bits=24`、`cdaccept`、`cdedge=`），Java 工具不提问，而是以相同信息停止，须在参数中给出该选项。

## 与 C++ 工具的一致性

Java 代码移植自 `src/` 中的 C++ 参考实现，移植过程中不修改 C++ 代码。每一步移植都用两者对相同输入编码和解码，并逐字节比较结果。可能影响输出的浮点运算一律使用 `LMath`，它与 C++ 工具的 `lmath::` 结果完全相同。

## 许可

与 C++ 代码相同：解码器和公共工具为 Apache-2.0；编码器文件为双重许可：GPL-3.0-or-later 与作者的商业许可，使用者任选其一（见 [COMMERCIAL.md](../COMMERCIAL.md)）。每个文件在其 SPDX 文件头中写明许可。
