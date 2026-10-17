// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// Prints the IMDCT self-test hash of this build (to be stored as SELFTEST_REF in src/mdct_tb.h from a reference build).
#include <cstdio>
#include "../src/mdct_tb.h"
int main() { printf("0x%016llxull\n", (unsigned long long)mdct::selftestHash()); return 0; }
