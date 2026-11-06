# LAE Noise Marker

[English](README.md)（以英文版为准）

LAE Noise Marker 检测母带中的平稳噪声和固定窄线，并由用户标定需要去除的部分。其结果写入标定文件（`.laenoise`，见 [SPEC_LAENOISE.zh.md](SPEC_LAENOISE.zh.md)）。LAE Lossy（LAEY）编码器读取该文件，并将其频率层中被标定的格子编码为静音（一个格子为该层网格中的一个 MDCT 帧 × 8 个频点，见 [SPEC_LAENOISE.zh.md](SPEC_LAENOISE.zh.md) 第 3 节）。CD 频带内的标定仅被列出。噪声标定仅适用于 LAEY。

**LAE Noise Marker 仅进行识别和标定。** 它从不去除声音，也不具备导出功能：没有任何命令或按钮会写出经过清理的母带副本。试听片段每个位置最长 10 秒。如用户将标定文件接入确实会去除声音的其他软件，则属用户本人的选择和责任。

## 软件包内容

| 文件 | 作用 |
|---|---|
| `lae_noise`（Windows 上为 `lae_noise.exe`） | 命令行程序，包含子命令 `scan`、`spectro`、`preview`、`preview-all`、`mark`、`show` 和 `level` |
| `noise_panel.html` | 面板；可在任何较新的浏览器（Chrome、Edge、Firefox、Safari）中离线打开；提供中文版和英文版 |
| `SPEC_LAENOISE.md` / `.zh.md` | 标定文件格式规范 |
| `LICENSE` | Apache-2.0 |

本程序以标准 C++17 编写，适用于 Windows、Linux 和 macOS（macOS 尚未核对）。其设计目标是在各平台上产生逐字节相同的候选和标定文件；已在 Windows x64 和 x86、Linux x86-64（GCC、Clang）、Linux x86 32 位以及 Linux ARM64（GCC、Clang）上进行核对。

## 开始之前

- **先对齐。** 标定应在编码器最终接收的母带上进行。对于 LAE，运行 `lae_enc plan`，确认对齐结果，然后在对齐后的母带上进行标定。标定文件通过 SHA-256 哈希值识别母带，因此在该文件任何其他版本上所做的标定均会被拒绝。
- **输入：** 立体声 WAV 文件，16 位、24 位或 32 位 PCM，或 32 位浮点。

## 步骤

**1. 扫描。** `scan` 命令列出所有持续 10 s 及以上的平稳噪声和固定窄线。短时突发不予列出。

```
lae_noise scan master.wav scan.txt candidates.laenoise
```

`scan.txt` 为报告；`candidates.laenoise` 以标定文件的形式包含相同的候选。此阶段尚未标定任何内容：候选仅为建议。

**2. 生成显示数据和试听片段。**

```
lae_noise spectro master.wav spectro.json
lae_noise preview master.wav candidates.laenoise previews
```

对于每个候选，向 `previews` 写入三个文件：`pos_NNN_original.wav`（该时段原样）、`pos_NNN_silenced.wav`（该时段中属于该候选的格子已静音）和 `pos_NNN_marked.wav`（仅包含将被静音的内容）。每个试听片段最多覆盖该时段的前 10 s，两侧各附 1 s 上下文。写入同一文件夹的 `preview_check.txt` 确认这些格子以外的内容均未改变。`lae_noise preview-all master.wav <file.laenoise> <folder> <t0 s> <t1 s>` 写出一个所有位置同时静音的片段（`all_original.wav`、`all_silenced.wav`、`all_marked.wav`），并进行相同的核对。

**3. 在面板中选择。** 打开 `noise_panel.html`，载入 `spectro.json`、`candidates.laenoise` 和 `previews` 文件夹。
- 所有候选初始均为未勾选状态；勾选需要去除的候选。
- 如需手动添加区域，可在频谱图上拖出一个矩形，或输入时间和频率。该区域向内吸附到完整的格子。
- 使用每一行中的三个按钮试听各试听片段。
- **保存标定文件** 仅写入已勾选的位置，例如保存为 `master.laenoise`。

面板从不处理声音；它仅播放试听文件。

**4. 核对文件。**

```
lae_noise show master.wav master.laenoise
```

该命令列出各位置，并确认该文件属于此母带。

**5. 编码。**

```
lae_enc master.wav out.wav 3 noise=master.laenoise
```

其中 `3` 为编码器模式（[README](../../README.zh.md) 第 11 节）；`g` 作为附加参数添加，例如 `lae_enc master.wav out.wav 3 noise=master.laenoise g`。编码器将其频率层（CD 层以上的超声频带）中被标定的格子编码为静音。CD 频带（频率层起点以下）内的标定由编码器列出；该处的音频不予改变。使用可选的 `g`（或 `g=b`、`g=c`、`g=d`）时，编码器还会去除这些格子中残留的 CD 层升采样镜像。这会使文件增大，编码器会打印增大的量。

**6. 测量（可选）。**

```
lae_noise level decoded.wav master.laenoise
```

该命令报告任何相同长度的文件（母带、解码文件或副本）中被标定格子的电平。与编码器和 `show` 不同，它仅核对长度和采样率，不核对 SHA-256，以便能够测量解码文件。

## 不使用面板

`lae_noise mark master.wav out.laenoise <t0 s> <t1 s> <f0 Hz> <f1 Hz> [...]` 根据数值写出标定文件。每个范围向内吸附到完整的格子，并打印吸附后的范围。

## 编译

本程序以标准 C++17 编写，不需要其他库。使用 MSVC 时，应使用 `/fp:strict`；使用 GCC 或 Clang 时，应使用 `-ffp-contract=off`；在 32 位 x86 上，应使用 SSE2。编译 LAE 代码库中的 `src/lae_noise.cpp`，其中已包含扫描器。

许可证：本文为 CC-BY-4.0；程序和面板为 Apache-2.0。
