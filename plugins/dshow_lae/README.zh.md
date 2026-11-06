# lae_source.ax：DirectShow 的 LAE 源滤镜

[English](README.md) | **中文**（以英文版为准）

`lae_source.ax` 是供 PotPlayer、MPC-BE、MPC-HC、Windows Media Player（旧版）及其他 DirectShow 播放器使用的 DirectShow 源滤镜（Windows x64）。已测试 PotPlayer 和 MPC-BE（见[测试](#tested-windows-11)），未测试 MPC-HC 和 Windows Media Player。它可打开两类 LAE 文件：

- **LAES**（LAE 无损；WAV、RF64 或 FLAC 形式），包括其中的无损母带。
- **LAEY**（LAE 有损；WAV，容器版本 5）。

该滤镜输出未压缩 PCM，支持跳转，采样率为所播放内容的采样率（见[播放内容](#what-plays)）：

- 2 声道（LAE 文件为立体声）。
- 16 位样本，或以 WAVE_FORMAT_EXTENSIBLE 形式置于 32 位容器中的 24 位和 32 位样本。

不采用打包为 3 字节的 24 位样本，因为 MPC-BE 的音频链路会将其变为噪声。该滤镜依据 DirectShow 接口文档为本项目编写，未使用 base classes，也未使用第三方代码。

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

播放由 `HKCU\Software\LAE\DirectShow` 下的 DWORD 注册表值控制。若未设置任何值，滤镜以母带自身的采样率播放母带。在 LAES 文件中下列限制所允许的母带和采样率档位中，滤镜播放采样率最高者，其次为字长最长者，母带优先于采样率档位；若 `maxbits` 小于核心母带的字长且无其他内容符合条件，则以基础采样率播放 16 位派生母带。

| 值 | 含义 |
|---|---|
| `layers` | 0：全部层 / 母带；1：CD 层和位深层（基础采样率）；2：仅 CD 层 |
| `maxrate` | 最高采样率，单位 Hz；0 = 不限（例如，对于无法接受更高采样率的声卡或渲染器，设为 96000） |
| `maxbits` | 仅适用于 LAES：最高字长；0 = 不限，16、24 或 32 |

## 规则（与 foobar2000 插件相同）

- 母带仅在完整解码且其 SHA-256 一致之后，才作为该母带播放。某一文件内容首次打开时，一次性校验全部母带，并显示一个小进度窗口。对于 Grand（10 分钟，4 个母带），在测试机上约需 1 分钟。结果按整个文件的 SHA-256 存储于 `%APPDATA%\LAE\laes_verified.txt`，因此以后打开仅需几秒（文件检查）。
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
