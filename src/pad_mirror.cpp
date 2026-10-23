// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// mirror-pad an s32 stereo raw file by P samples at both ends (encoder side; the decoder trims P from the output)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "enc_io.h"
int main(int argc, char** argv) {
    std::vector<int32_t> x = encio::readAll<int32_t>(argv[1]); long n = encio::count(x.size()) / 2; long P = atol(argv[3]);
    std::vector<int32_t> y((n + 2 * P) * 2);
    for (long i = 0; i < n + 2 * P; i++) { long j = i - P; if (j < 0) j = -j; if (j >= n) j = 2 * (n - 1) - j; y[i * 2] = x[j * 2]; y[i * 2 + 1] = x[j * 2 + 1]; }
    encio::writeVec(argv[2], y);
    return 0;
}
