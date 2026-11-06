# 为 LAE（Layered Audio Extension）做贡献

[English](CONTRIBUTING.md)（以英文版为准）

<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC0-1.0
-->

感谢您的贡献。在提交拉取请求（PR）之前，请阅读本文档。不符合以下规则的贡献无法合并。

## 1. 贡献许可与签署

**贡献许可。** 贡献按其所贡献的仓库部分的许可证接受：

- **代码**（源代码、工具、测试脚本、构建文件，包括编码器侧源代码）：Apache License, Version 2.0（`LICENSES/Apache-2.0.txt`），依该许可证第 5 节的规定；
- **文档与规范**：该文档的许可证（`CC-BY-4.0`；第 2 节所列项目文件为 `CC0-1.0`）；
- **数据**（一致性测试向量、测试数据、测量数据）：`CC0-1.0`。

无需签署贡献者许可协议，亦不转让版权。

**Developer Certificate of Origin。** 每个提交必须带有签署行：

```
Signed-off-by: Your Name <your.address@example.org>
```

- `git commit -s` 会自动添加该行。
- 签署即表示您认证 Developer Certificate of Origin 1.1 版（https://developercertificate.org）。
- 请使用与您在 `AUTHORS` 中条目相同的姓名。您可使用 GitHub 的 no-reply 地址。
- 未签署的提交无法合并。

**您的权利。** 您保留对您贡献的版权。您将在 Git 历史和 `AUTHORS` 中署名。

**贡献的使用。** 由于代码按 Apache-2.0 贡献，作者 mtq0605 可将您的代码贡献纳入编码器侧源代码；编码器侧源代码按 GPL-3.0-or-later 以及商业许可证（`COMMERCIAL.md`）分发。

**雇佣关系。** 如果您在受雇过程中编写贡献，请确保您有权按上述条款提交。Developer Certificate of Origin 对此有要求。

## 2. 项目文件的许可证

项目文件按下列许可证分发。贡献按其所贡献部分的许可证接受（第 1 节）；对于编码器侧源代码，即为 Apache-2.0。

| 文件 | 许可证（SPDX） |
|---|---|
| `spec/lossy/SPEC.md`、`spec/lossless/DESIGN.md`、`spec/` 中的其他文件 | `CC-BY-4.0` |
| `README.md`、`README.zh.md`、`CHANGELOG.md`、`docs/`；`plugins/` 中的 README 文件和图像 | `CC-BY-4.0` |
| `src/` 中的解码器侧源代码：`lae_dec.cpp`、`lae_decoder.h`、`lae_api.h`、`lae_api.cpp`、`part_stream.h`、`partition_format.h`、`mdct_tb.h`、`mdct_tables.h`、`halfband.h`、`wavio.h`、`fft.h`、`gen_suite.cpp`、`cut_wav.cpp`、`cmp_wav.cpp` | `Apache-2.0` |
| `src/` 中的其他源代码：`lae_cdx.cpp`、`lae_cdx.h`、`lae_cdx_read.h`、`iso9660.h`、`align_hash.h`、`frac_delay.h` | `Apache-2.0` |
| `src/` 中的编码器侧源代码以及 Java 库的编码器文件（完整列表见 `COMMERCIAL.md`） | `GPL-3.0-or-later OR LicenseRef-LAE-Commercial` |
| `tools/`、`tests/` 中的测试脚本、构建文件、`.github/` | `Apache-2.0` |
| `plugins/foo_input_lae/` 中的 foobar2000 组件：`foo_input_lae.cpp`、`build_fb2k.cmd` | `Apache-2.0` |
| `tests/` 中的一致性测试向量、测试数据和向量清单；`data/` | `CC0-1.0` |
| `LICENSE`、`NOTICE`、`AUTHORS`、`PATENTS.md`、`COMMERCIAL.md`、`CONTRIBUTING.md`、`REUSE.toml` | `CC0-1.0` |

`src/` 中的其他源代码遵循解码器侧文件的规则（第 3 节和第 4 节）：它们带有 Apache-2.0 文件头，且不依赖编码器侧文件。

**新文件。** 对于每个新的源文件，请在 PR 中说明其属于解码器侧还是编码器侧。最终归属由维护者决定。

## 3. 署名与文件头

**AUTHORS。** 在您的第一个 PR 中，将您自己添加到 `AUTHORS`，包括以下内容：

- 您的署名名称（可使用化名）；
- 您的 GitHub 用户名。

添加电子邮件地址为可选项。

**SPDX 文件头。** 每个文件均带有 SPDX 文件头，或由 `REUSE.toml` 覆盖（例如一致性测试向量、数据文件、图像以及无文件头的 Markdown 文件）。请使用与文件相符的文件头。

解码器侧源代码、工具和测试脚本：

```c
// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
```

编码器侧源代码：

```c
// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
```

规范与文档（Markdown；采用 CC0-1.0 的项目文件使用相同形式，但为 `CC0-1.0`；其他 Apache-2.0 文件按该文件的注释语法使用 Apache-2.0 文件头）：

```markdown
<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC-BY-4.0
-->
```

**您的版权行。** 当您编写了文件的实质性部分时，请添加您自己的版权行。

- **实质性**指新增函数、新增表格、新增规范章节，或约 15 行及以上的非平凡代码。
- **形式：** `SPDX-FileCopyrightText: <year> <your attribution name>`。添加电子邮件地址为可选项。
- **位置：** 位于现有各行之下。
- **年份：** 您首次为该文件做出贡献的年份。允许使用 `2026-2028` 这样的范围。

**不得删除**他人的版权行。

**检查。** 在提交 PR 之前，请运行 `reuse lint`（https://reuse.software）。

## 4. 来源规则（禁止第三方代码）

1. **禁止复制代码。** 不得复制、翻译或近似改写任何其他来源的代码。这包括：
   - 其他编解码器和音频库；
   - 宽松许可代码和公有领域代码；
   - 问答网站上的代码；
   - 教科书中的代码。
2. **禁止新增依赖。** 未经事先在 issue 中讨论，不得添加依赖。
3. **欢迎采用论文和标准中的算法。**
   - 依据其描述进行实现。
   - 在注释中注明来源：`// Method: <author, title, year>`。
   - 不得将其中的文字复制到 `spec/` 或 `docs/` 中。
4. **净室。** 如果您近期阅读过同一技术的其他实现的源代码，请在 PR 中说明。维护者可请他人编写该部分。
5. **允许使用 AI 助手**，但须满足以下条件。
   - 在 PR 描述中说明是否使用了 AI 辅助：无、少量（补全）或大量（起草代码或文本）。
   - 您必须理解并能够解释您提交的每一行。
   - 不得要求工具复现或模仿其他项目的代码。
   - 如果您的工具具有屏蔽与公开代码相匹配的建议的设置，请启用该设置。
6. **解码器与编码器之间的边界。**
   - 解码器侧文件不得包含或依赖任何编码器侧文件。这包括 `partition.h`。
   - 编码器侧文件可使用解码器侧文件。

## 5. 规范、码流和一致性变更

- **先提出格式变更。** 在提交对格式、解码过程或编码器编码工具的变更之前，请在 issue 中提出并说明其动机。
- **新的容器版本。** 任何改变任一一致性测试向量解码器输出的变更，均需要新的容器版本。
- **标注规范变更。** 将对 `spec/` 的每项变更标注为**规范性**（改变一致性）或**资料性**。
- **描述输出变更。** 如果某项变更影响 LAE 码流或解码器输出，请在 PR 中描述其影响。
- **解释向量变更。** 解释对一致性测试向量或向量清单的每项变更。

## 6. 测试素材

1. **仅限生成的信号。** 使用正弦波、扫频、噪声、脉冲、多音信号和合成瞬态等信号。
   - 不得添加录音、采样、音乐或语音，包括您自己的录音。
   - 不得添加第三方测试文件，包括标准测试声音集。
2. **可复现的生成。** 每个提交的向量必须能够由仓库中的代码复现。请在 PR 中说明生成该向量的命令。

## 7. 拉取请求检查清单

- [ ] 每个提交均带有 `Signed-off-by:` 行
- [ ] 我已列入 `AUTHORS`
- [ ] 所有新文件均有 SPDX 文件头，并在我所贡献部分为实质性时添加了我的版权行
- [ ] 解码器侧文件不依赖编码器侧文件
- [ ] 无第三方代码，无新增依赖
- [ ] 已在 PR 描述中说明 AI 辅助情况
- [ ] 已标注规范变更，并已解释码流或向量变更（如有）
- [ ] `reuse lint` 通过

## 8. 联系方式

通过 GitHub 联系。

- **一般问题、提案和商业许可咨询。** 在 LAE 仓库中提交 issue。不得在 issue 中填写保密信息；详细事宜随后另行安排（见 `COMMERCIAL.md`）。
- **安全问题。** 不得提交公开 issue。请使用仓库的私密漏洞报告功能（Security 选项卡）；见 `SECURITY.md`。
