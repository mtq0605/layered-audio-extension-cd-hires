# LAE 噪声标定

[English](README.md)

LAE 噪声标定在母带里找出长时间平稳的噪声和固定的窄线，由你标出想去掉的部分。
- 结果写成标定文件（`.laenoise`，格式见 [SPEC_LAENOISE.zh.md](SPEC_LAENOISE.zh.md)）。
- LAE 编码器读这个文件，把标定的格子编成静音。

**LAE 噪声标定只识别和标定。**
- 它从不删除声音，也没有导出功能：没有任何命令或按钮会写出"处理过的母带"。
- 试听片段每个位置最长 10 秒。
- 如果你把标定文件接到会删除声音的其他软件上，那是你自己的选择和责任。

## 包里有什么

| 文件 | 作用 |
|---|---|
| `lae_noise`（Windows 上是 `lae_noise.exe`） | 命令行程序：`scan`、`spectro`、`preview`、`preview-all`、`mark`、`show`、`level` |
| `noise_panel.html` | 面板：用任何较新的浏览器打开（Chrome、Edge、Firefox、Safari），不用联网，中英两版 |
| `SPEC_LAENOISE.md` / `.zh.md` | 标定文件格式 |
| `LICENSE` | Apache-2.0 |

程序是标准 C++17，可在 Windows、Linux、macOS 上编译。设计上同一个母带在各平台得到逐字节相同的候选和标定文件；已核对的是 Windows x64 / x86 和 Linux x64（GCC、Clang、32 位）。

## 开始之前

- **先对齐。** 标定要做在编码器最终拿到的那个母带上。
  - 用 LAE 时：先运行 `lae_enc plan`，确认对齐结果，再在对齐后的母带上标定。
  - 标定文件用 SHA-256 认母带，在别的版本上做的标定会被拒绝。
- **输入：** 立体声 WAV，PCM 16 / 24 / 32 bit，或 32 bit 浮点。

## 步骤

**1. 扫描。** 列出所有持续 10 秒以上的平稳噪声和固定窄线，短促的突发不列。

```
lae_noise scan 母带.wav scan.txt candidates.laenoise
```

- `scan.txt` 是报告；`candidates.laenoise` 是同样的候选，写成标定文件的格式。
- 这时还没有标定任何东西，候选只是建议。

**2. 生成显示数据和试听片段。**

```
lae_noise spectro 母带.wav spectro.json
lae_noise preview 母带.wav candidates.laenoise previews
```

每个候选在 `previews` 文件夹里得到三个文件：
- `pos_NNN_original.wav`：这一段原样；
- `pos_NNN_silenced.wav`：这一段里标定的格子静音后；
- `pos_NNN_marked.wav`：只有会被静音的部分。

`preview_check.txt` 核对标定格子以外没有任何变化。

**3. 在面板里选。** 打开 `noise_panel.html`，载入 `spectro.json`、`candidates.laenoise` 和 `previews` 文件夹。
- 每个候选一开始都不勾选，勾上你要去掉的。
- 手动加区域：在频谱图上拖一个矩形，或者输入时间和频率。范围会向内吸附到完整的格子。
- 每一行有三个按钮，用来试听。
- "保存标定文件"只保存勾选的位置。

面板本身不处理声音，只播放试听文件。

**4. 核对文件。**

```
lae_noise show 母带.wav 母带.laenoise
```

列出所有位置，并确认文件属于这个母带。

**5. 编码。**

```
lae_enc 母带.wav out.wav 3 noise=母带.laenoise
```

- 编码器把频率层（CD 层以上的超声频带）里标定的格子编成静音。
- CD 频带里的标定，编码器会列出来，但不改动。
- 可选的 `g`（以及 `g=b`、`g=c`、`g=d`）会再去掉这些格子里 CD 层升采样留下的镜像。文件因此变大，编码器会打印大了多少。

**6. 测量（可选）。**

```
lae_noise level 解码.wav 母带.laenoise
```

量出任何同样长度的文件（母带、解码结果、副本）在标定格子里的电平。

## 不用面板

`lae_noise mark 母带.wav out.laenoise <t0 秒> <t1 秒> <f0 Hz> <f1 Hz> [...]` 按数字写出标定文件。每个范围向内吸附到完整的格子，并打印吸附后的范围。

## 编译

标准 C++17，不用其他库。
- MSVC 加 `/fp:strict`；GCC / Clang 加 `-ffp-contract=off`；32 位 x86 用 SSE2。
- 编译 `src/lae_noise.cpp` 即可，扫描器已包含在内。

许可证：本文 CC-BY-4.0；程序和面板 Apache-2.0。
