# LAEY 扩展包：一个 CD 层，多个母带

[English](PACKS.md)（以英文版为准）

一个 LAEY 文件有一个 CD 层（48 kHz 系列中为核心层）和一个或多个**扩展包**。每个扩展包由一个母带对着这个 CD 层整套生成，包与包之间没有任何共用。旧的播放器和解码器播放 CD 层加第一个包（主体），不受影响。规范性文本：[spec/lossy/SPEC.md](../spec/lossy/SPEC.zh.md) 第 5.6 节。

## 1. 编码

编码分两步，与使用独立 CD 时相同（README 第 4.3 节）：位置由你确认。

### 第一步：编码器搜索每个另给母带的位置

```
lae_enc master1.wav out.wav 3 s1 master2=master2.wav master3=master3.wav
```

对每个另给的母带，编码器运行白化搜索和普通搜索，打印建议的位置（一行 `ALIGN_SUGGEST …`），然后停止，不写出任何内容。

### 第二步：写上位置并编码

```
lae_enc master1.wav out.wav 3 s1 master1name="原版母带" master2=master2.wav master2align=+0.8459282 master2name="电子管放大器母带" master3=master3.wav master3align=0 master3name="模拟磁带母带"
```

| 选项 | 含义 |
|---|---|
| `master2=` … `master9=` | 另给的母带，编号不能跳过。每个得到一个扩展包：`master2` 是包 1，`master3` 是包 2，依此类推。 |
| `master<n>align=<offset>` | 该母带在 CD 层时间轴上的位置，写法同 `cdalign=`（以它自己的样本为单位，可以带小数）。 |
| `master<n>name=<name>` | 播放器为这个包显示的名称，最多 255 字节。不写就没有名称。 |
| `master1name=<name>` | 播放器为主体（第一个母带，包 0）显示的名称，最多 255 字节。不写时输出不变。 |
| `master<n>accept` | 相当于 `cdaccept`：剩余时间差在 −40 dB 之外时继续，出现漂移或检查块不匹配的警告之后也继续。 |
| `master<n>edge=pad` 或 `cut` | 该母带在 CD 层之外还有声音时的选择，相当于 `cdedge=`。不选就不编码。 |

CD 层的来源：

- 不写 `cd=`：CD 层由母带 1 派生。
- 写 `cd=your_cd.wav`：CD 层就是这张 CD；母带 1 先对齐到它（`cdalign=` 等选项，与以前相同）。

模式、方案（`s1` / `s2`）、`zs` 和其他编码选项对所有包相同。

### 须知

- 各母带的采样率可以不同，但须属于同一系列（44.1 kHz 或 48 kHz）。
- CD 层本身的样本从不改动。对齐需要时（母带按小数个样本移动，或 `…edge=pad`），在 CD 层两端补数字静音：取各母带所需的最大值，只补一次，每个母带补同样的静音。
- 每多一个母带，就多一次对齐和一次完整编码。
- 大小：每个包只增加它自己的扩展。母带与 CD 层的差别越大，它的包越大。在演示作品的 10 秒片段上，CD 层约 1.76 MB，每个包 0.6–1.1 MB。
- 母带相对 CD 层有速度漂移（例如磁带的抖晃），或部分检查块与 CD 层不匹配时，编码器给出警告；只有手动给出位置并确认（`master<n>accept`，或在控制台中回答），才会继续，从不依据它自己的搜索继续。
- `g` 可以与另给的母带一起使用（每个包按自己的母带处理）。另给的母带不能与 `noise=`、`resume` 或 `cdcheck` 同时使用，整盘规划器 `lae_enc plan` 也不能直接加入另给的母带。

## 2. 解码

```
lae_dec in.wav out.wav [full|16|cd24] [output rate] [pack=<number>]
```

- 不写 `pack=`，或写 `pack=0`：CD 层加第一个包，与以前完全相同。
- `pack=1`、`pack=2`……：CD 层加那个包。
- 文件中有另外的包时，`lae_dec` 列出每个包的采样率和名称（存有主体名称时也列出）。
- 输出采样率和 24 位、16 位、cd24 路径在选中的包之内照常使用。
- `lae_udec info <file>` 把每个包及其路径作为选项列出（`pack1_full_176400` 等）。`lae_udec dec --pick=<rate>:<bits>` 只在主体的各项中选择；其他包用 `--pick=pack:<k>` 或该包某一项的名称选择。
- 自 2026-11-15 起，`lae_dec` 在写出任何内容之前，把所选包的每条熵编码流都解到最后；`lae_udec verify` 把每个包（主体和其余各包）都解到最后，每包打一行结果。播放器在第一次选中某个其他包时检查它。

## 3. 其他形式

- FLAC 形式（输出文件名以 `.flac` 结尾）：携带所有包。
- CD-Extra 封装（`lae_cdx`）：扩展文件原样携带所有包；所需空间随包增加。

## 4. 支持情况

| 组件 | 支持 |
|---|---|
| `lae_enc`、`lae_dec`（x64、x86、Linux x64 和 ARM64） | 每个包的编码和解码 |
| `liblae`（API 8 起：`lae_pack_count`、`lae_pack_info`、`lae_select_pack`）和 `lae_udec` | 列出和选择包 |
| foobar2000、VLC、Kodi 和 DirectShow 插件 | 选择包；foobar2000 按存储的名称列出各包及其层（见各插件 README） |
| Java 库 | 编码器、解码器和相同的接口，与 C++ 工具逐字节相同 |
