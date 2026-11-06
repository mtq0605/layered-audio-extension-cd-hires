# liblae_plugin.dll：VLC 的 LAE 输入模块

[English](README.md) | **中文**

VLC 媒体播放器 3.0（Windows 64 位）的输入（demux）模块。它打开 LAE 文件：**LAES**（LAE 无损；WAV、RF64 或 FLAC 形式）及其无损母带，和
**LAEY**（LAE v5 WAV）。VLC 打开每个文件时都会问各输入模块"这是不是你的"，所以本模块**不论扩展名**（`.lae`、`.flac`、`.wav`）都能接
LAE 文件，母带不用改名就能放。别的文件它只读结构（毫秒级）就退回，普通 FLAC、WAV 照旧由 VLC 自己的模块放。输出 PCM：双声道，16 位，
或 24 位、32 位放在 32 位字里，采样率与所放的版本相同，可以跳转。

## 安装（当前用户，不要管理员权限）

运行 `lae_vlc_setup-<版本>.exe`：模块放到 `%LOCALAPPDATA%\Programs\LAE\VLC`，把这个文件夹加进你的环境变量 `VLC_PLUGIN_PATH`（VLC 3
也从这里列的文件夹加载模块；原有的条目保留），并在 Windows 的"已安装的应用"里加一项（在那里卸载）。装好后重新打开 VLC。

也可以手动：把 `liblae_plugin.dll` 复制到 VLC 的 `plugins\demux` 文件夹（已安装的 VLC 需要管理员权限）。

## 放什么

VLC > 工具 > 偏好设置 > 显示设置：全部 > 输入/编解码器 > 分流器 > LAE（都不设 = 母带，按它自己的采样率）：

| 设置 | 含义 |
|---|---|
| What plays（`--lae-layers`） | 全部层 / 母带；CD 层 + 深度层（基础采样率）；只要 CD 层 |
| Highest sample rate（`--lae-maxrate`） | 不限、192 kHz、96 kHz 或 48 kHz |
| Highest word length（`--lae-maxbits`） | LAES：不限、16、24 或 32 位 |

## 规则（与其他 LAE 播放器相同）

- 母带只有在完整解码并且 SHA-256 相符之后才按母带放。一个文件内容第一次打开时，一遍校验全部母带，带进度对话框。结果按整个文件的
  SHA-256 记在 `%APPDATA%\LAE\laes_verified.txt`，这是和 foobar2000、Kodi、DirectShow 插件共用的文件夹，所以在任一播放器里校验过的文件，
  在这里直接播放。
- 跳转从包含该时间的块开始。块位置记在 `%APPDATA%\LAE\laes_index\`（每 16 块一个，几十 kB），从不写进媒体文件。

## 测试（Windows 11，VLC 3.0.24 64 位，2026-11-04）

用 VLC 的串流输出（`--sout "#std{access=file,mux=wav,...}"`）把模块交出的数据原样写成 WAV，与 `laes_tool dec` / `lae_dec` 逐样本比较：

- 15 秒的 LAES 文件（`.lae`，母带 352.8 kHz / 24 位）：完全相同。采样率上限设 96 kHz：放 88.2 kHz 档，完全相同。
- Grand（10:09，FLAC 形式，仍叫 `.flac`，4 个母带）：第一次打开校验了全部 4 个母带，然后放母带 1（352.8 kHz / 32 位），完全相同。
  从 300 秒开始放：从那个采样起完全相同。
- LAEY 文件（`.wav`）：与 `lae_dec` 完全相同（全部层，352.8 kHz）。
- 普通 WAV：不接，由 VLC 自己的 WAV 模块放。
- VLC 的串流输出每个文件都丢掉最后 40～50 毫秒（用它自己的 WAV 模块也一样）；比较覆盖到那里为止。
- VLC 正常播放时会用自己的重采样器让声卡跟上时钟，所以送到声卡的不是逐位相同的。这是 VLC 对所有格式的做法；本模块交出的样本是精确的。

## 编译

```bat
plugins\vlc_lae\build_vlc.cmd <VLC sdk 文件夹> <输出文件夹>
plugins\vlc_lae\build_setup.cmd <VLC sdk 文件夹> <输出文件夹>
```

VS2022 Build Tools，x64。`sdk` 文件夹（头文件和 `libvlccore.lib`）在 VLC 64 位的 7z 包里（`vlc-3.0.x-win64.7z`；zip 包里没有）。LAE 解码器用
各自核对过的编译选项（`lae_api` `/fp:precise`，`laes_api` `/fp:strict`）。模块对应一个 VLC 系列（3.0），VLC 4 需要重新编译。

## 许可

源文件是 Apache-2.0。模块二进制文件含有 VLC 插件头文件里的代码（LGPL-2.1-or-later），整体按 LGPL-3.0-or-later 分发。安装程序是 Apache-2.0。
