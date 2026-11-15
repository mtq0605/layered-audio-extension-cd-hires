# liblae_plugin.dll：VLC 的 LAE 输入模块

[English](README.md) | **中文**（以英文版为准）

本模块是 VLC 媒体播放器 3.0（Windows，64 位）的输入（demux）模块。它打开 LAE 文件：**LAE Lossless (LAES)** 文件（WAV、RF64 或 FLAC 形式）及其无损母带，以及 **LAE Lossy (LAEY)** 文件（容器版本 5，WAV 或 FLAC 形式）。VLC 打开每个文件时，都会向每个输入模块询问该文件。本模块接受**任意扩展名**（`.lae`、`.flac`、`.wav`）的 LAE 文件，因此母带无须重命名即可播放。对于其他文件，本模块只读取其结构即予拒绝，耗时为毫秒级，普通 FLAC 和 WAV 文件仍由 VLC 自身的模块播放。本模块输出双声道 PCM，样本为 16 位，或为置于 32 位字中的 24 位或 32 位样本，采样率与所播放内容的采样率相同，并支持跳转。

## 本次更新的变化

- 也能打开 FLAC 形式的 LAEY 文件（[spec/lossy/SPEC.md](../../spec/lossy/SPEC.zh.md) 第 3.4 节）；文件按内容识别。
- LAEY 扩展包（同一规范第 5.6 节）：选项"LAEY 扩展包"（`lae-pack`）选择扩展包。它和 `lae_dec pack=` 一样从 0 数起：0 = 主体，即母带 1 的扩展包；n = 母带 n + 1 的扩展包（foobar2000 和 Kodi 的界面从 1 数起，那里的“扩展包 n + 1”就是这里的 n；日志也按它们的数法称呼扩展包，并附上 lae-pack 的值：“扩展包 3（lae-pack 2）无法使用；播放扩展包 1”，扩展包 1 就是主体）。文件中没有该扩展包时播放主体；该扩展包无法使用时也播放主体，只有这时才在日志中提示。不会自动选择任何扩展包。
- 选项"LAEY 层"（`lae-layers`）提供"全部层""仅频率层""仅位深层"和"仅 CD 层/核心层"。
- LAES 有效载荷版本 2（[spec/lossless/SPEC.md](../../spec/lossless/SPEC.zh.md) 第 10.3 节）：没有其他设置时，播放文件中存储的默认母带；文件没有默认母带时播放核心母带。
- 母带及其参照链的 SHA-256 全部相符后才播放该母带；核对结果按文件内容保存。派生 16 位版本从跳转位置开始播放，逐位相同。
- 英文（`-en`）和中文（`-zh`）两个独立版本。VLC 3.0 的模块选项只显示英文。

## 安装（当前用户，无须管理员权限）

运行 `lae_vlc_setup-<version>.exe`。安装程序：

- 将模块放入 `%LOCALAPPDATA%\Programs\LAE\VLC`；
- 将该文件夹添加到您的环境变量 `VLC_PLUGIN_PATH` 中（VLC 3 也从该变量所列的文件夹中加载模块；原有条目保留）；
- 在 Windows 的已安装应用列表中添加一项，可从该处卸载本模块。

安装后重新启动 VLC。

也可以手动安装本模块：将 `liblae_plugin.dll` 复制到 VLC 的 `plugins\demux` 文件夹中。对于已安装的 VLC，此操作需要管理员权限。

## 播放内容

设置位于 VLC > 工具 > 偏好设置 > 显示设置：全部 > 输入/编解码器 > 分流器 > LAE。若未作任何设置，LAEY 文件以母带采样率播放全部层；LAES 文件播放其默认母带（发行方编码时标出的母带；没有标出，或有效载荷未通过检查时，播放核心母带）。不按采样率或位深自动选择母带（[spec/lossless/SPEC.md](../../spec/lossless/SPEC.zh.md) 第 10.3 节），与其他 LAE 播放器的规则相同。限制只在这个母带之内起作用：播放它不高于最高采样率的最高采样率档位（采样率档位是降至较低采样率的母带）；字长超过最高位深时，播放该条目自己的派生 16 位版本；默认母带没有合适的档位时，播放核心母带。对于 LAES，"仅位深层"播放默认母带在基础采样率下的档位，"仅 CD 层/核心层"播放核心母带。

| 设置 | 含义 |
|---|---|
| LAEY 层（`--lae-layers`） | 0 全部层（LAES：母带）；1 仅位深层（基础采样率）；2 仅 CD 层/核心层；3 仅频率层（LAES：同 0） |
| LAEY 扩展包（`--lae-pack`） | 0 = 主体；n = 母带 n + 1 的扩展包（见上文） |
| 最高采样率（`--lae-maxrate`） | 不限、192 kHz、96 kHz 或 48 kHz |
| LAES 最高位深（`--lae-maxbits`） | 不限、16 位、24 位或 32 位 |

## 规则（与其他 LAE 播放器相同）

- 母带只有在完整解码且与其 SHA-256 哈希值相符之后，才作为该母带播放。某一文件内容首次打开时，在一遍处理中校验全部母带，并显示带“取消”按钮的进度对话框。若某一母带校验失败，或校验被取消，则播放核心母带，并由 VLC 的错误对话框说明；取消后不记录任何结果，下次打开文件时重新校验。校验结果存储在 `%APPDATA%\LAE` 中的文件 `laes_verified.txt` 里，以整个文件的 SHA-256 哈希值为键（每次打开时首先计算该哈希值，大文件需数秒）。文件夹 `%APPDATA%\LAE` 与 foobar2000、Kodi 和 DirectShow 播放器共用，因此在其中任一播放器中校验过的文件，在 VLC 中可立即播放。
- 跳转从包含目标时间的块开始。块位置（每 16 块记录一个；数十 kB）存储在 `%APPDATA%\LAE\laes_index\` 中，从不写入媒体文件。

## 测试（Windows 11，VLC 3.0.24 64 位，2026-11-04）

使用 VLC 的串流输出（`--sout "#std{access=file,mux=wav,...}"`）将本模块的输出原样写入 WAV 文件，并与 `laes dec` / `lae_dec` 的输出逐样本比较：

- 一个 15 秒的 LAES 文件（`.lae`；母带 352.8 kHz / 24 位）：完全相同。将采样率上限设为 96 kHz 时，播放母带的 88.2 kHz 采样率档位：完全相同。
- Grand（LAE 发布版本中的一首演示曲目；10:09；FLAC 形式，仍命名为 `.flac`；4 个母带）：首次打开时校验了全部 4 个母带，随后播放母带 1（352.8 kHz / 32 位）：完全相同。从 300 s 处开始播放：自该样本起完全相同。
- 一个 LAEY 文件（`.wav`）：与 `lae_dec` 的输出完全相同（全部层，352.8 kHz）。
- 一个普通 WAV 文件：本模块不接受；由 VLC 自身的 WAV 模块播放。
- VLC 的串流输出会丢弃每个文件的最后 40-50 ms，使用其自身的 WAV 模块时亦然。比较覆盖至该点之前的全部内容。
- 正常播放时，VLC 运行其自身的重采样器以使声卡保持时钟同步，因此到达声卡的信号并非逐位精确。VLC 对所有格式均如此处理；本模块输出的样本是精确的。

## 构建

```bat
plugins\vlc_lae\build_vlc.cmd <VLC sdk folder> <output folder>
plugins\vlc_lae\build_setup.cmd <VLC sdk folder> <output folder>
```

构建需要 VS2022 Build Tools（x64）。`sdk` 文件夹（头文件和 `libvlccore.lib`）包含在 VLC 的 64 位 7z 包（`vlc-3.0.x-win64.7z`）中，zip 包中则没有。LAE 解码器使用各自经过验证的编译器选项进行编译（`lae_api` 使用 `/fp:precise`，`laes_api` 使用 `/fp:strict`）。本模块的每次构建只对应一个 VLC 系列（3.0）；VLC 4 将需要重新构建。

## 许可

源文件采用 Apache-2.0 许可。模块二进制文件包含来自 VLC 插件头文件的代码（LGPL-2.1-or-later），其整体按 LGPL-3.0-or-later 分发。许可全文见 [LICENSES/LGPL-3.0-or-later.txt](../../LICENSES/LGPL-3.0-or-later.txt)；它纳入了 GNU GPL 第 3 版的条款，后者的全文见 [LICENSES/GPL-3.0-or-later.txt](../../LICENSES/GPL-3.0-or-later.txt)。安装程序（`setup/setup.cpp`）采用 Apache-2.0 许可；其所安装的模块保持上述许可。
