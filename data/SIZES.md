<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC0-1.0
-->

# File sizes by mode, scheme and sample rate

Generated from [loop10_v5_release_candidate.csv](loop10_v5_release_candidate.csv) (see [README.md](README.md) for how it was measured).
Values are **medians in MB per minute of stereo music** (1 MB = 10^6 bytes; 10-second excerpts x 6). "CD" is the CD layer as
uncompressed PCM; "depth" and "freq." are the two extension layers; "total" = CD + depth + freq. The CD layer is identical in all modes.

- Measured on the v5 release candidate: the published encoder gives frequency-layer sizes 0.01-0.3 % smaller.
- **Recordings** (88.2-192 kHz, anonymised) are the representative figures. **Synthetic** signals are stress tests (sweeps, noise,
  square waves, transients ...) and show extremes, not typical music; they are the only data at 352.8 and 384 kHz.

## Recordings

### 88.2 kHz (6 recordings) - CD layer 10.6 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.3 | 3.1 | 15.0 | 1.3 | 1.7 | 13.6 |
| 1 | 1.3 | 3.3 | 15.1 | 1.3 | 1.9 | 13.8 |
| 3 | 1.3 | 3.3 | 15.2 | 1.3 | 2.0 | 13.9 |
| 5 | 1.3 | 3.3 | 15.2 | 1.3 | 2.1 | 13.9 |
| keep | 1.3 | 3.4 | 15.2 | 1.3 | 2.1 | 14.0 |
| keep20 | 2.8 | 3.4 | 16.6 | 2.8 | 2.1 | 15.4 |

### 96 kHz (5 recordings) - CD layer 11.5 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.4 | 5.5 | 18.6 | 1.4 | 4.1 | 17.1 |
| 1 | 1.4 | 5.5 | 18.7 | 1.4 | 4.3 | 17.2 |
| 3 | 1.4 | 5.6 | 18.7 | 1.4 | 4.3 | 17.3 |
| 5 | 1.4 | 5.7 | 18.7 | 1.4 | 4.5 | 17.4 |
| keep | 1.4 | 5.7 | 18.8 | 1.4 | 4.6 | 17.6 |
| keep20 | 3.0 | 5.7 | 20.3 | 3.0 | 4.6 | 19.1 |

### 176.4 kHz (6 recordings) - CD layer 10.6 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.3 | 6.7 | 18.6 | 1.3 | 3.5 | 15.4 |
| 1 | 1.3 | 6.9 | 18.8 | 1.3 | 3.8 | 15.6 |
| 3 | 1.3 | 6.9 | 18.8 | 1.3 | 3.9 | 15.7 |
| 5 | 1.3 | 6.9 | 18.8 | 1.3 | 3.9 | 15.8 |
| keep | 1.3 | 7.0 | 18.9 | 1.3 | 4.0 | 15.9 |
| keep20 | 2.7 | 7.0 | 20.3 | 2.7 | 4.0 | 17.3 |

### 192 kHz (1 recordings) - CD layer 11.5 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.4 | 12.5 | 25.5 | 1.4 | 8.8 | 21.8 |
| 1 | 1.4 | 12.6 | 25.6 | 1.4 | 9.1 | 22.1 |
| 3 | 1.4 | 12.8 | 25.7 | 1.4 | 9.3 | 22.3 |
| 5 | 1.4 | 12.9 | 25.9 | 1.4 | 9.5 | 22.5 |
| keep | 1.4 | 13.0 | 26.0 | 1.4 | 9.7 | 22.6 |
| keep20 | 3.0 | 13.0 | 27.6 | 3.0 | 9.7 | 24.2 |

## Synthetic test signals

### 88.2 kHz (10 signals) - CD layer 10.6 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.4 | 1.7 | 13.7 | 1.4 | 1.2 | 13.2 |
| 1 | 1.6 | 1.8 | 15.7 | 1.6 | 1.4 | 15.4 |
| 3 | 1.7 | 1.8 | 15.9 | 1.7 | 1.4 | 15.5 |
| 5 | 1.8 | 1.9 | 16.0 | 1.8 | 1.5 | 15.7 |
| keep | 1.8 | 1.9 | 16.0 | 1.8 | 1.5 | 15.7 |
| keep20 | 2.9 | 1.9 | 16.4 | 2.9 | 1.5 | 16.3 |

### 96 kHz (10 signals) - CD layer 11.5 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.5 | 1.9 | 14.6 | 1.5 | 1.4 | 14.2 |
| 1 | 1.7 | 2.0 | 17.0 | 1.7 | 1.5 | 16.8 |
| 3 | 1.7 | 2.0 | 17.1 | 1.8 | 1.5 | 17.0 |
| 5 | 1.8 | 2.0 | 17.2 | 1.8 | 1.6 | 17.2 |
| keep | 1.8 | 2.1 | 17.3 | 1.8 | 1.6 | 17.3 |
| keep20 | 3.2 | 2.1 | 17.6 | 3.2 | 1.6 | 17.6 |

### 176.4 kHz (10 signals) - CD layer 10.6 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.5 | 4.7 | 16.6 | 1.5 | 3.4 | 15.3 |
| 1 | 1.7 | 4.8 | 18.5 | 1.7 | 3.6 | 17.3 |
| 3 | 1.8 | 4.8 | 18.5 | 1.8 | 3.7 | 17.3 |
| 5 | 1.8 | 4.9 | 18.5 | 1.8 | 3.8 | 17.4 |
| keep | 1.9 | 5.0 | 18.5 | 1.9 | 3.9 | 17.4 |
| keep20 | 3.1 | 5.0 | 19.3 | 3.1 | 3.9 | 18.1 |

### 192 kHz (10 signals) - CD layer 11.5 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.5 | 5.0 | 17.8 | 1.5 | 3.6 | 16.4 |
| 1 | 1.8 | 5.2 | 19.6 | 1.8 | 4.1 | 18.5 |
| 3 | 1.9 | 5.3 | 19.7 | 1.9 | 4.2 | 18.6 |
| 5 | 2.0 | 5.5 | 19.7 | 2.0 | 4.3 | 18.6 |
| keep | 2.1 | 5.5 | 19.8 | 2.1 | 4.4 | 18.6 |
| keep20 | 3.3 | 5.5 | 20.6 | 3.3 | 4.4 | 19.5 |

### 352.8 kHz (10 signals) - CD layer 10.6 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.4 | 10.3 | 22.3 | 1.4 | 7.6 | 19.6 |
| 1 | 1.9 | 10.8 | 24.2 | 1.9 | 8.0 | 21.6 |
| 3 | 2.1 | 11.0 | 24.3 | 2.1 | 8.2 | 21.8 |
| 5 | 2.2 | 11.2 | 24.4 | 2.2 | 8.5 | 21.9 |
| keep | 2.3 | 11.2 | 24.5 | 2.3 | 8.7 | 22.0 |
| keep20 | 3.5 | 11.2 | 25.2 | 3.5 | 8.7 | 22.7 |

### 384 kHz (10 signals) - CD layer 11.5 MB/min

| Mode | s1 depth | s1 freq. | s1 total | s2 depth | s2 freq. | s2 total |
|---|---|---|---|---|---|---|
| max | 1.6 | 11.1 | 23.9 | 1.6 | 8.4 | 21.2 |
| 1 | 2.1 | 11.8 | 25.5 | 2.1 | 9.3 | 22.9 |
| 3 | 2.1 | 12.2 | 25.6 | 2.1 | 9.5 | 23.0 |
| 5 | 2.1 | 12.4 | 25.7 | 2.1 | 9.8 | 23.2 |
| keep | 2.2 | 12.8 | 26.1 | 2.2 | 10.3 | 23.6 |
| keep20 | 3.6 | 12.8 | 27.0 | 3.6 | 10.3 | 24.5 |

