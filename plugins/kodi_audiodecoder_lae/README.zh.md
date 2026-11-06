# audiodecoder.lae —— Kodi 的 LAE 解码插件

[English](README.md) | **中文**

在 Kodi 21（Omega）里播放 LAE 文件：**LAES**（LAE 无损；WAV、RF64 或 FLAC 形式）及其中的无损母带，以及 **LAEY**（LAE v5 WAV）。

## 文件：扩展名 `.lae`

Kodi 只把解码插件声明的扩展名交给它，而且不允许插件把文件退回给 Kodi 自带的解码器。插件如果接管 `.flac` 或 `.wav`，就得自己解码所有
FLAC 和 WAV 文件。所以本插件只接管格式自己的扩展名 **`.lae`**：

- 把 LAE 文件（`.flac` 或 `.wav`）改名为 `.lae`，就能在 Kodi 里放其中的母带；
- 保持 `.flac` / `.wav` 时，Kodi 用自带解码器放它的 核心母带（可以是 CD 兼容的）（这本来就是 LAE 在任何播放器里的设计）。

## 安装

Kodi → 插件 → 从 zip 文件安装 → `audiodecoder.lae-<版本>.zip`（Kodi 会要求一次允许未知来源）。Windows x64。

## 放哪一个

设置（插件 → 我的插件 → 音频解码器 → LAE decoder → 设置）：

| 设置 | 选项 |
|---|---|
| 播放内容 | 全部层 / 母带；CD 层 + 深度层（基础采样率）；只放 CD 层 |
| 最高采样率 | 不限；192 kHz；96 kHz |
| LAES：最高位深 | 不限；16；24；32 bit |

LAES：先取不超过上限的最高采样率，再取最长字长；母带优先于采样率档（与 foobar2000 插件相同）。

## 规则（与 foobar2000 插件相同）

- 母带要整个解码一遍、和文件里存的 SHA-256 一致，才按母带播放。每份文件内容第一次播放时，一遍校验全部母带，带进度窗口（Grand，10 分钟、
  4 个母带：测试机上约 2 分钟）；结果按整个文件的 SHA-256 记住（`laes_verified.txt`；Windows 上在 `%APPDATA%\LAE`，与其他 LAE 播放器共用；其他系统在插件数据文件夹），以后直接播放。没通过的母带不放，改放核心母带。
- 跳转从包含该时间的块开始；块位置存在 `laes_index\<SHA-256>.idx`（每 16 块记一个，几十 kB；从不写进音频文件）。
- 输出：16 位、24 位（放在 32 位容器里）或 32 位整数 PCM，采样率就是所选内容自己的，不做任何改动。

## 测试（Kodi 21.3 x64，便携副本）

- 一个 15 秒的 LAES 文件和 Grand（10:09，4 个母带，FLAC 形式）改名为 `.lae`：第一次播放先校验母带，然后放母带（352.8 kHz / 32 bit）；
  拖到 60 % 立即到 6:07；第二次播放不再校验（打开后约 0.8 秒开始）。
- 这里没测：其他平台（Linux、macOS、安卓要各自编译）、LAEY 文件、网络路径。

## 编译

```bat
plugins\kodi_audiodecoder_lae\build_kodi.cmd <kodi-dev-kit include 文件夹> <输出文件夹>
```

kodi-dev-kit 头文件来自 Kodi 官方仓库（`xbmc/addons/kodi-dev-kit/include`，标签 `21.3-Omega`）。VS2022 生成工具，x64。
LAE 解码器用各自验证过的选项（liblae `/fp:precise`，LAES 解码器 `/fp:strict`）。

## 许可

本插件源码：Apache-2.0。编进二进制的 kodi-dev-kit 头文件是 GPL-2.0-or-later，所以插件二进制整体按 GPL-3.0-or-later 分发。
