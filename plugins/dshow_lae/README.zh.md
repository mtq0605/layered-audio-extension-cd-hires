# lae_source.ax —— DirectShow 的 LAE 源滤镜

[English](README.md) | **中文**

给 PotPlayer、MPC-BE、MPC-HC、Windows Media Player（旧版）等 DirectShow 播放器用的源滤镜（Windows x64）。打开 LAE 文件：**LAES**
（LAE 无损；WAV、RF64 或 FLAC 形式）及其中的无损母带，以及 **LAEY**（LAE v5 WAV）。输出未压缩 PCM（2 声道；16 位，或 24 / 32 位的
WAVE_FORMAT_EXTENSIBLE），采样率就是所放内容自己的，支持跳转。按 DirectShow 接口文档为本项目编写（不用 base classes，没有第三方代码）。

## 安装（当前用户，不需要管理员权限）

运行 `lae_dshow_setup-<版本>.exe`：把滤镜放到 `%LOCALAPPDATA%\Programs\LAE\DirectShow`、注册，并在 Windows 的"应用"列表里加一项（在那里卸载）。或者手动：

```bat
regsvr32 lae_source.ax
```

注册滤镜（COM 类、DirectShow 滤镜目录、扩展名 `.lae` 的源）；没有管理员权限时只注册到当前用户（`HKCU\Software\Classes`）。`.flac` / `.wav` 形式的 LAE 文件在任何播放器里照样放核心母带（可以是 CD 兼容的）；要放母带，可以改名为
`.lae`，或者在播放器里加上这个滤镜：

- PotPlayer（64 位）：参数选项（F5）→ 滤镜 → 全局滤镜优先权 → 添加外部滤镜 → `lae_source.ax` → 强制使用（已测试）。
- MPC-BE / MPC-HC（64 位）：选项 → 外部滤镜 → 添加滤镜 → LAE source → 优先。

卸载：`regsvr32 /u lae_source.ax`。

## 放哪一个

注册表 `HKCU\Software\LAE\DirectShow` 下的 DWORD 值（都不设 = 母带本身的采样率）：

| 值 | 含义 |
|---|---|
| `layers` | 0 全部层 / 母带；1 CD 层 + 深度层（基础采样率）；2 只放 CD 层 |
| `maxrate` | 最高采样率（Hz），0 = 不限（例如声卡或渲染器接受不了更高时设 96000） |
| `maxbits` | LAES：最高位深，0 = 不限，16、24 或 32 |

## 规则（与 foobar2000 插件相同）

- 母带要整个解码一遍、和 SHA-256 一致，才按母带播放。每份文件内容第一次打开时，一遍校验全部母带，带一个小进度窗口（Grand，10 分钟、
  4 个母带：测试机上约 1 分钟）；结果按整个文件的 SHA-256 记在 `%APPDATA%\LAE\laes_verified.txt`，以后打开只要几秒（文件检查）。
- 跳转从包含该时间的块开始；块位置存在 `%APPDATA%\LAE\laes_index\`（每 16 块记一个，几十 kB；从不写进音频文件）。

## 测试（Windows 11，用系统自带音频渲染器的 DirectShow 图）

- 一个 15 秒的 LAES 文件和 Grand（10:09，4 个母带，FLAC 形式）：载入、播放、跳到 60 % 立即生效；Grand 第一次打开校验全部母带（63 秒），
  第二次 3 秒打开。
- PotPlayer 64 位（2026-11-03）：作为外部滤镜加入（强制使用）；15 秒的 LAES 文件以 352.8 kHz / 24 bit 放出母带（PotPlayer 显示 PCM 16934 kb/s）；
  播放中跳转正常。系统自带的音频渲染器不接受 352.8 kHz（可用 maxrate 限制）。
- MPC-BE 1.9.2 x64（便携版，2026-11-03），外部滤镜设为优先：24 位打包成 3 字节时它的音频链路放出噪声（声卡回录：每秒约 15000 次过零，音乐约 750）；改为 24 位放在 32 位容器里后正常放音乐（回录每秒约 700–830 次，与 PotPlayer 相同）。交出的字节与解出的母带一致（24 位为 ×256）。

## 编译

```bat
plugins\dshow_lae\build_dshow.cmd <输出文件夹>
```

VS2022 生成工具，x64；Windows SDK 自带的 DirectShow 头文件和 `strmiids.lib`。LAE 解码器用各自验证过的选项（liblae `/fp:precise`，
LAES 解码器 `/fp:strict`）。

## 许可

Apache-2.0。
