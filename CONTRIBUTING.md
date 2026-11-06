<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC0-1.0
-->

# Contributing to LAE (Layered Audio Extension)

Thank you for contributing. Please read this document before you open a
pull request (PR). Contributions that do not follow these rules cannot be
merged.

## 1. Inbound licence and sign-off

**Inbound licence.** Every contribution is accepted under the Apache
License, Version 2.0 (`LICENSES/Apache-2.0.txt`), regardless of the
licence of the file that you change. By submitting a contribution, you
license it under Apache-2.0, as described in Section 5 of that licence.

**Developer Certificate of Origin.** Every commit must carry a sign-off
line:

```
Signed-off-by: Your Name <your.address@example.org>
```

- `git commit -s` adds this line automatically.
- By signing off, you certify the Developer Certificate of Origin, version
  1.1 (https://developercertificate.org).
- Use the same name as in your `AUTHORS` entry. You may use your GitHub
  no-reply address.
- Commits without a sign-off cannot be merged.

**Your rights.** You retain the copyright in your contribution. You are
credited in the Git history and in `AUTHORS`.

**Use of contributions.** The author, mtq0605, may include your
contribution in the encoder-side sources, which are distributed under
GPL-3.0-or-later and under a commercial licence (`COMMERCIAL.md`).

**Employment.** If you write a contribution in the course of your
employment, make sure that you have the right to submit it under these
terms. The Developer Certificate of Origin requires this.

## 2. Licences of the project files

Contributions are accepted under Apache-2.0 (Section 1). The project files
are distributed under the following licences.

| Files | Licence (SPDX) |
|---|---|
| `spec/lossy/SPEC.md`, `spec/lossless/DESIGN.md`, other files in `spec/` | `CC-BY-4.0` |
| `README.md`, `README.zh.md`, `CHANGELOG.md`, `docs/`; the README files and images in `plugins/` | `CC-BY-4.0` |
| Decoder-side sources in `src/`: `lae_dec.cpp`, `lae_decoder.h`, `lae_api.h`, `lae_api.cpp`, `part_stream.h`, `partition_format.h`, `mdct_tb.h`, `mdct_tables.h`, `halfband.h`, `wavio.h`, `fft.h`, `gen_suite.cpp`, `cut_wav.cpp`, `cmp_wav.cpp` | `Apache-2.0` |
| Other sources in `src/`: `lae_cdx.cpp`, `lae_cdx.h`, `lae_cdx_read.h`, `iso9660.h`, `align_hash.h`, `frac_delay.h` | `Apache-2.0` |
| Encoder-side sources in `src/` and the encoder files of the Java library (complete list in `COMMERCIAL.md`) | `GPL-3.0-or-later OR LicenseRef-LAE-Commercial` |
| `tools/`, test scripts in `tests/`, build files, `.github/` | `Apache-2.0` |
| foobar2000 component in `plugins/foo_input_lae/`: `foo_input_lae.cpp`, `build_fb2k.cmd` | `Apache-2.0` |
| Conformance vectors, test data and the vector manifest in `tests/`; `data/` | `CC0-1.0` |
| `LICENSE`, `NOTICE`, `AUTHORS`, `PATENTS.md`, `COMMERCIAL.md`, `CONTRIBUTING.md`, `REUSE.toml` | `CC0-1.0` |

**New files.** For each new source file, state in the PR whether it
belongs to the decoder side or to the encoder side. The maintainer decides
the final assignment.

## 3. Attribution and file headers

**AUTHORS.** In your first PR, add yourself to `AUTHORS` with the
following:

- your attribution name (a pseudonym is acceptable);
- your GitHub username.

Adding your e-mail address is optional.

**SPDX headers.** Every file carries an SPDX header, except conformance
vectors and data files, which are covered by `REUSE.toml`. Use the header
that matches the file.

Decoder-side sources, tools and test scripts:

```c
// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
```

Encoder-side sources:

```c
// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
```

Specification and documentation (Markdown):

```markdown
<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC-BY-4.0
-->
```

**Your copyright line.** Add your own copyright line when you author a
substantial part of a file.

- **Substantial** means a new function, a new table, a new specification
  section, or roughly 15 or more lines of non-trivial code.
- **Form:** `SPDX-FileCopyrightText: <year> <your attribution name>`.
  Adding your e-mail address is optional.
- **Place:** below the existing lines.
- **Year:** the year of your first contribution to that file. A range such
  as `2026-2028` is allowed.

**Never remove** another person's copyright line.

**Check.** Run `reuse lint` (https://reuse.software) before you open the
PR.

## 4. Provenance rules (no third-party code)

1. **No copied code.** Do not copy, translate or closely paraphrase code
   from any other project. This includes:
   - other codecs and audio libraries;
   - permissive and public-domain code;
   - code from Q&A sites;
   - code from textbooks.
2. **No new dependencies.** Do not add a dependency without prior
   discussion in an issue.
3. **Algorithms from papers and standards are welcome.**
   - Implement them from their description.
   - Cite the source in a comment: `// Method: <author, title, year>`.
   - Do not copy prose from them into `spec/` or `docs/`.
4. **Clean room.** If you have recently read the source code of another
   implementation of the same technique, state this in the PR. The
   maintainer may ask someone else to write that part.
5. **AI assistants are allowed** under the following conditions.
   - State in the PR description whether you used AI assistance: none,
     minor (completion) or substantial (drafted code or text).
   - You must understand, and be able to explain, every line that you
     submit.
   - Do not ask a tool to reproduce or imitate another project's code.
   - If your tool has a setting that blocks suggestions matching public
     code, enable it.
6. **Boundary between decoder and encoder.**
   - Decoder-side files must not include, or depend on, any encoder-side
     file. This includes `partition.h`.
   - Encoder-side files may use decoder-side files.

## 5. Specification, bitstream and conformance changes

- **Raise format changes first.** Before you submit a change to the
  format, to the decoding process or to the encoder's coding tools, raise
  it in an issue and state its motivation.
- **New container version.** Any change that alters the decoder output
  for any conformance vector requires a new container version.
- **Label specification changes.** Mark each change to `spec/` as
  **normative** (it changes conformance) or **informative**.
- **Describe output changes.** If a change affects the LAE bitstream or
  the decoder output, describe the effect in the PR.
- **Explain vector changes.** Explain every change to a conformance vector
  or to the vector manifest.

## 6. Test material

1. **Generated signals only.** Use signals such as sine waves, sweeps,
   noise, impulses, multitone signals and synthetic transients.
   - Do not add recordings, samples, music or speech, including your own
     recordings.
   - Do not add third-party test files, including standard test-sound
     collections.
2. **Reproducible generation.** Every committed vector must be
   reproducible from code in the repository. State the command that
   produced it.

## 7. Pull request checklist

- [ ] Every commit carries a `Signed-off-by:` line
- [ ] I am listed in `AUTHORS`
- [ ] SPDX headers present on all new files, and my copyright line added
      where my part is substantial
- [ ] Decoder-side files do not depend on encoder-side files
- [ ] No third-party code and no new dependencies
- [ ] AI assistance stated in the PR description
- [ ] Specification changes labelled, and bitstream or vector changes
      explained (if any)
- [ ] `reuse lint` passes

## 8. Contact

Contact is through GitHub.

- **General questions, proposals and commercial licence enquiries.** Open
  an issue in the LAE repository. Do not put confidential information in
  an issue; details are arranged afterwards (see `COMMERCIAL.md`).
- **Security issues.** Do not open a public issue. Use the private
  vulnerability reporting of the repository (the Security tab); see
  `SECURITY.md`.
