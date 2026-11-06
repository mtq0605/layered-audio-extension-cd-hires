# audiodecoder.lae：Kodi 的 LAE 解码器

[English](README.md) | **中文**（以英文版为准）

本插件用于在 Kodi 21（Omega）中播放 LAE 文件。它支持 WAV、RF64 或 FLAC 形式的 **LAES**（LAE Lossless）文件（包括其中的无损母带），以及容器版本 5 WAV 形式的 **LAEY**（LAE Lossy）文件。

## 文件：扩展名 `.lae`

Kodi 只把具有解码插件所声明扩展名的文件交给该插件，并且不允许插件将文件交还给 Kodi 的内置解码器。因此，声明 `.flac` 或 `.wav` 的插件将不得不自行解码所有 FLAC 和 WAV 文件。为此，本插件只声明该格式自身的扩展名 **`.lae`**：

- 若要在 Kodi 中播放 LAE 文件（`.flac` 或 `.wav`，包括 RF64 形式）中的母带，应将该文件重命名为 `.lae`。
- 扩展名为 `.flac` 或 `.wav` 时，该文件由 Kodi 的内置解码器播放，播放的是核心母带（对 LAEY 而言为 CD 层）。核心母带可以是 CD 兼容的（这正是 LAE 在任何播放器中的预期工作方式）。

## 安装

在 Kodi 中选择 插件 → 从 zip 文件安装 → `audiodecoder.lae-<version>.zip`。Kodi 会询问一次是否允许未知来源。平台：Windows x64。

## 播放内容

设置位于 插件 → 我的插件 → 音频解码器 → LAE decoder → 配置：

| 设置 | 取值 |
|---|---|
| 播放内容 | 全部层 / 母带；CD 层 + 位深层（基础采样率）；仅 CD 层 |
| 最高采样率 | 不限；192 kHz；96 kHz |
| LAES：最大字长 | 不限；16；24；32 bit |

对于 LAES，先选择不超过上限的最高采样率，再选择最长的字长。母带优先于采样率档位（采样率档位是将某一母带降至其采样率的二分之一、四分之一或八分之一所得的结果），与 foobar2000 组件相同。

## 规则（与 foobar2000 组件相同）

- 母带只有在被完整解码并与文件中存储的 SHA-256 相符之后，才作为该母带播放。某一文件内容首次播放时，在一遍处理中校验其全部母带，并显示进度窗口（Grand 是 LAE 发布版本中的一首演示曲目：10 分钟，4 个母带，在测试机上约需 2 分钟）。校验结果以整个文件的 SHA-256 为键存储在 `laes_verified.txt` 中（在 Windows 上位于 `%APPDATA%\LAE`，与其他 LAE 播放器共用；在其他平台上位于插件的数据文件夹），以便此后的播放能够立即开始。未通过校验的母带不予播放，改为播放核心母带。
- 跳转从包含目标时间的块开始。块位置存储在 `laes_index\<SHA-256>.idx` 中（在 Windows 上位于 `%APPDATA%\LAE`；在其他平台上位于插件的数据文件夹）（每 16 个块记录一次；数十 kB；从不写入媒体文件）。
- 输出：16 位、24 位（置于 32 位容器中）或 32 位整数 PCM，采样率为所选母带或层的采样率，不作任何修改。

## 测试情况（Kodi 21.3 x64，便携副本）

- 一个 15 秒的 LAES 文件和 Grand（10:09，4 个母带，FLAC 形式），扩展名均为 `.lae`：首次播放时，先校验母带，然后播放母带（352.8 kHz / 32 bit）；跳转到约 60 % 处立即生效（Kodi 显示 6:07）；第二次播放无需校验即开始（打开后约 0.8 s）。
- 此处未测试：其他平台（Linux、macOS 和 Android 需要各自的构建）、LAEY 文件以及网络路径。

## 构建

```bat
plugins\kodi_audiodecoder_lae\build_kodi.cmd <kodi-dev-kit include folder> <output folder>
```

kodi-dev-kit 头文件取自 Kodi 仓库（`xbmc/addons/kodi-dev-kit/include`，标签 `21.3-Omega`）。要求：VS2022 Build Tools，x64。LAE 解码器使用其各自的编译选项进行编译，即 [docs/BUILD.md](../../docs/BUILD.zh.md) 第 1 节中为其验证过的选项（liblae：`/fp:precise`；LAES 解码器：`/fp:strict`）。

## 许可

本插件的源代码：Apache-2.0。编译进二进制文件的 kodi-dev-kit 头文件按 GPL-2.0-or-later 许可；因此，插件二进制文件整体按 GPL-3.0-or-later 分发。
