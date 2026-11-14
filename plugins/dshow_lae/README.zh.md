# lae_source.ax：DirectShow 的 LAE 源滤镜

[English](README.md) | **中文**（以英文版为准）

`lae_source.ax` 是供 PotPlayer、MPC-BE、MPC-HC、Windows Media Player（旧版）及其他 DirectShow 播放器使用的 DirectShow 源滤镜（Windows x64）。已测试 PotPlayer 和 MPC-BE（见[测试](#tested-windows-11)），未测试 MPC-HC 和 Windows Media Player。它可打开两类 LAE 文件：

- **LAES**（LAE 无损；WAV、RF64 或 FLAC 形式），包括其中的无损母带。
- **LAEY**（LAE 有损；容器版本 5，WAV 或 FLAC 形式）。

该滤镜输出未压缩 PCM，支持跳转，采样率为所播放内容的采样率（见[播放内容](#播放内容)）：

- 2 声道（LAE 文件为立体声）。
- 16 位样本，或以 WAVE_FORMAT_EXTENSIBLE 形式置于 32 位容器中的 24 位和 32 位样本。

不采用打包为 3 字节的 24 位样本，因为 MPC-BE 的音频链路会将其变为噪声。该滤镜依据 DirectShow 接口文档为本项目编写，未使用 base classes，也未使用第三方代码。

## 本次更新的变化

- 也能打开 FLAC 形式的 LAEY 文件（[spec/lossy/SPEC.md](../../spec/lossy/SPEC.zh.md) 第 3.4 节）；文件按内容识别。
- LAEY 扩展包（同一规范第 5.6 节）：`HKCU\Software\LAE\DirectShow` 中的注册表值 `pack`（DWORD）选择扩展包。它和 `lae_dec pack=` 一样从 0 数起：0 = 主体，即母带 1 的扩展包；n = 母带 n + 1 的扩展包（foobar2000 和 Kodi 的界面从 1 数起，那里的“扩展包 n + 1”就是这里的 n）。文件中没有该扩展包时播放主体（0）。不会自动选择任何扩展包。
- LAES 有效载荷版本 2（[spec/lossless/SPEC.md](../../spec/lossless/SPEC.zh.md) 第 10.3 节）：没有其他设置时，播放文件中存储的默认母带；文件没有默认母带时播放核心母带。
- 母带及其参照链的 SHA-256 全部相符后才播放该母带；核对结果按文件内容保存（保存在 `%APPDATA%\LAE`，与其他 LAE 插件共用）。派生 16 位版本从跳转位置开始播放，逐位相同。
- 滤镜及其安装程序分英文（`-en`）和中文（`-zh`）两个独立版本。

## 安装（当前用户，无需管理员权限）

运行 `lae_dshow_setup-<version>.exe`。安装程序将滤镜放入 `%LOCALAPPDATA%\Programs\LAE\DirectShow`，完成注册，并在 Windows 的已安装应用列表中添加一项。可在该列表中卸载滤镜。也可以手动注册滤镜：

```bat
regsvr32 lae_source.ax
```

该命令注册 COM 类、DirectShow 滤镜目录中的条目，并将该滤镜注册为 `.lae` 扩展名的源。无管理员权限时，注册仅对当前用户有效（`HKCU\Software\Classes`）。

扩展名为 `.flac` 或 `.wav` 的 LAE 文件在任何播放器中仍播放其核心母带（可以是 CD 兼容的）。若要播放其母带，可将文件改名为 `.lae`，或在播放器中添加该滤镜：

- PotPlayer（64 位）：参数选项（F5）→ 滤镜 → 全局滤镜优先权 → 添加外部滤镜 → `lae_source.ax` → 强制使用（已测试）。
- MPC-BE / MPC-HC（64 位）：选项 → 外部滤镜 → 添加滤镜 → LAE source → 优先。

注销滤镜时，运行 `regsvr32 /u lae_source.ax`。

## 播放内容

播放由 `HKCU\Software\LAE\DirectShow` 下的 DWORD 注册表值控制。若未设置任何值，LAEY 文件以母带采样率播放全部层；LAES 文件播放其默认母带（出版方编码时标出的母带；没有标出，或载荷未通过检查时，播放核心母带）。不按采样率或位深自动选择母带（[spec/lossless/SPEC.md](../../spec/lossless/SPEC.zh.md) 第 10.3 节）。下列限制只在这个母带之内起作用：`maxrate` 选出它不高于限制的最高采样率档位；字长超过 `maxbits` 时，播放该条目自己的派生 16 位版本；默认母带没有合适的档位时，播放核心母带。

| 值 | 含义 |
|---|---|
| `layers` | 0：全部层 / 母带；1：仅位深层（CD 层或核心层 + 位深层，基础采样率；LAES：默认母带在基础采样率下的档位）；2：仅 CD 层或核心层（LAES：核心母带）；3：仅频率层（CD 层或核心层 + 频率层，16 位；LAES：同 0） |
| `maxrate` | 最高采样率，单位 Hz；0 = 不限（例如，对于无法接受更高采样率的声卡或渲染器，设为 96000） |
| `maxbits` | 仅适用于 LAES：最高字长；0 = 不限，16、24 或 32 |

## 规则（与 foobar2000 插件相同）

- 母带仅在完整解码且其 SHA-256 一致之后，才作为该母带播放。某一文件内容首次打开时，一次性校验全部母带，并显示一个带“取消”按钮的小进度窗口。对于 Grand（10 分钟，4 个母带），在测试机上约需 1 分钟。结果按整个文件的 SHA-256 存储于 `%APPDATA%\LAE\laes_verified.txt`，因此以后打开仅需几秒（文件检查）。点“取消”后，这一次播放核心母带，不记录任何结果：所选母带要在完整校验通过后才能播放，下次打开文件时重新校验。凡是以核心母带代替所选母带播放时（校验已取消、母带没有通过校验或无法校验），都会弹出提示框说明。
- 跳转从包含所请求时间的块开始。块位置（每 16 个块记录一次，数十 kB）存储于 `%APPDATA%\LAE\laes_index\`，从不写入媒体文件。

## 测试（Windows 11）

- 使用测试程序 `test_dshow.cpp`（以 `IGraphBuilder::Render` 构建的 DirectShow 图；未记录其所选用的渲染器），对一个 15 秒的 LAES 文件和 Grand（10:09，4 个母带，FLAC 形式）进行测试：两者均能载入、播放，并立即跳转至 60 %。Grand 首次打开时校验全部母带（63 秒）；第二次打开用时 3 秒。
- PotPlayer 64 位（2026-11-03），将滤镜作为外部滤镜添加（强制使用）：一个 15 秒的 LAES 文件以 352.8 kHz / 24 bit 播放其母带（PotPlayer 显示 PCM 16934 kb/s），播放中跳转正常。若播放器的音频渲染器不接受 352.8 kHz，可设置 `maxrate`。
- MPC-BE 1.9.2 x64（便携版，2026-11-03），将滤镜作为外部滤镜设为优先：采用打包为 3 字节的 24 位样本时，其音频链路播放出噪声。声卡回录显示每秒约 15000 次过零，而非约 750 次。采用置于 32 位容器中的 24 位样本时，可正常播放音乐，回录中每秒约 700-830 次过零，与 PotPlayer 相同。输出的字节与解码所得母带一致（24 位为 x 256）。

## 编译

```bat
plugins\dshow_lae\build_dshow.cmd <output folder>
```

要求：VS2022 Build Tools（x64），以及 Windows SDK 中的 DirectShow 头文件和 `strmiids.lib`。LAE 解码器以各自经过验证的选项编译：liblae 使用 `/fp:precise`，LAES 解码器使用 `/fp:strict`。

## 许可

Apache-2.0。
