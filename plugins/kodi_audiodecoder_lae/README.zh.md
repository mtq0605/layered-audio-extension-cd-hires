# audiodecoder.lae：Kodi 的 LAE 解码器

[English](README.md) | **中文**（以英文版为准）

本插件用于在 Kodi 21（Omega）中播放 LAE 文件。它支持 WAV、RF64 或 FLAC 形式的 **LAES**（LAE Lossless）文件（包括其中的无损母带），以及容器版本 5、WAV 或 FLAC 形式的 **LAEY**（LAE Lossy）文件。

## 本次更新的变化

- 也能解码 FLAC 形式的 LAEY 文件（[spec/lossy/SPEC.md](../../spec/lossy/SPEC.zh.md) 第 3.4 节）。和以前一样，Kodi 只把扩展名为 `.lae` 的文件交给本插件。
- LAEY 扩展包（同一规范第 5.6 节）：设置"LAEY 扩展包"（扩展包 1 … 扩展包 9）选择扩展包。"扩展包 n" 是母带 n 的扩展包，和 foobar2000 菜单一样从 1 数起；`lae_dec pack=`、DirectShow 和 VLC 从 0 数起，记作 n − 1（"扩展包 1" 就是主体，日志里写作 pack 0）。文件中没有该扩展包时播放扩展包 1。不会自动选择任何扩展包。
- 设置"LAEY 层"提供"全部层""仅频率层""仅位深层"和"仅 CD 层/核心层"。
- LAES 有效载荷版本 2（[spec/lossless/SPEC.md](../../spec/lossless/SPEC.zh.md) 第 10.3 节）：没有其他设置时，播放文件中存储的默认母带；文件没有默认母带时播放核心母带。
- 母带及其参照链的 SHA-256 全部相符后才播放该母带；核对结果按文件内容保存。派生 16 位版本从跳转位置开始播放，逐位相同。
- 英文（`-en`）和中文（`-zh`）两个安装包，各只含一种语言（`lang_pkg.ps1`，由 `build_kodi.cmd` 调用）。

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
| LAEY 层 | 全部层（LAES：母带）；仅位深层（CD 层或核心层 + 位深层，基础采样率）；仅 CD 层/核心层（LAES：核心母带）；仅频率层（LAES：同全部层） |
| 最高采样率 | 不限；192 kHz；96 kHz |
| LAES 最高位深 | 不限；16 位；24 位；32 位 |
| LAEY 扩展包 | 扩展包 1 … 扩展包 9（见上文） |

对于 LAES，播放文件的默认母带（出版方编码时标出的母带；没有标出，或载荷未通过检查时，播放核心母带）；不按采样率或位深自动选择母带（[spec/lossless/SPEC.md](../../spec/lossless/SPEC.zh.md) 第 10.3 节）。限制只在这个母带之内起作用：播放它不高于「最高采样率」的最高采样率档位（采样率档位是将某一母带降至其采样率的二分之一、四分之一或八分之一所得的结果）；字长超过「LAES 最高位深」时，播放该条目自己的派生 16 位版本；默认母带没有合适的档位时，播放核心母带。规则与 foobar2000 组件相同。

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
