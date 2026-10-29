// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-LAE-Commercial
// laes selftest (2026-10-29): regression test with synthetic signals only (no audio files, no external tools). Included by laes_tool.cpp
// after its helpers (Rng, wavBytes, readWav, readAll, writeAll, deriveCore, setT, derive16).
//   round trips: WAV form (16-bit core), FLAC form (automatic 24-bit core; 16 / 24 / 32-bit masters), 48 kHz family, 32-bit core,
//                fractional and negative T, unused low bits (FLAC wasted bits), short and odd lengths, derived 16-bit determinism
//   damage:      the LAES block with byte flips (CRC-32 must catch them), with flips behind a recomputed CRC (field checks and SHA-256),
//                truncation; the FLAC file with flips and truncation; random bytes into the FLAC subframe and residual parsers.
//                A damaged input must give an error (or a SHA-256 / MD5 mismatch), never a crash, a hang or wrong samples reported as good.
#pragma once
namespace selftest {
static int fails = 0;
static void report(const char* name, bool ok, const std::string& why = "") { printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name, ok || why.empty() ? "" : ": ", ok ? "" : why.c_str()); if (!ok) fails++; }
// one "mix" evaluated at any rate: tones up to 30 kHz (only those below half the rate) plus white noise, TPDF-quantised to bits,
// with `unused` low bits left zero (FLAC wasted bits)
static Pcm synth(int rate, int bits, double secs, uint64_t seed, int unused = 0) {
    Pcm p; p.rate = rate; p.bits = bits; const long n = (long)(secs * rate); Rng r(seed); const double sc = std::ldexp(1.0, bits - 1 - unused), pi = 3.14159265358979323846;
    const double f[5] = {440, 3150, 15000, 30000, 61.7}, a[5] = {0.30, 0.20, 0.05, 0.02, 0.25};
    for (int c = 0; c < 2; c++) { p.c[c].resize(n);
        for (long i = 0; i < n; i++) { const double t = (double)i / rate; double s = 0; for (int k = 0; k < 5; k++) if (f[k] < rate / 2.0) s += a[k] * std::sin(2 * pi * f[k] * t + c * 0.7 + k);
            s += 0.01 * r.uni(); const int64_t mx = (int64_t)sc - 1; p.c[c][i] = std::max(-mx, std::min(mx, (int64_t)std::floor(s * sc + r.uni() + r.uni() + 0.5))) * (1LL << unused); } }
    return p; }
static bool same(const Pcm& a, const Pcm& b) { return a.rate == b.rate && a.bits == b.bits && a.c[0] == b.c[0] && a.c[1] == b.c[1]; }
// encode -> container (WAV or FLAC, in memory and through a file for WAV) -> decode; every master must come back bit for bit
// Ts: T relative to the core (the encoder chooses storage and tier) for masters whose storage is -1; masters given a storage keep theirs
static bool roundTrip(std::vector<MasterIn> ms, const std::vector<double>& Ts, int base, int coreBits, bool flac, std::string& why, std::vector<uint8_t>* fileOut = nullptr, Pcm* coreOut = nullptr,
                      std::vector<MasterInfo>* infoOut = nullptr) {
    size_t src; bool dith; const Pcm core = deriveCore(ms, base, coreBits, src, dith); for (size_t i = 0; i < ms.size(); i++) if (ms[i].storage < 0) setT(ms[i], Ts[i]);
    std::vector<MasterInfo> info; std::string err; const std::vector<uint8_t> body = encodeLaes(core, base, ms, info, err, flac ? 1 : 0); if (!err.empty()) { why = "encode: " + err; return false; }
    Pcm core2; std::vector<uint8_t> body2;
    if (flac) { const std::vector<uint8_t> f = laesflac::writeFlac(core.c, base, core.bits, body); laesflac::Info fi; bool md5ok;
        const std::string e = laesflac::readFlac(f, fi, core2.c, body2, md5ok); if (!e.empty() || !md5ok) { why = "FLAC read: " + (e.empty() ? std::string("MD5") : e); return false; }
        core2.rate = fi.rate; core2.bits = fi.bps; if (fileOut) *fileOut = f; }
    else { const std::string path = (fs::temp_directory_path() / "laes_selftest.wav").string(); const std::vector<uint8_t> wb = wavBytes(core, &body);
        if (!writeAll(path, wb)) { why = "cannot write temp WAV"; return false; } const std::string e = readWav(path, core2, &body2); fs::remove(path); if (!e.empty()) { why = e; return false; } if (fileOut) *fileOut = wb; }
    if (!same(core, core2) || body != body2) { why = "container changed the core or the LAES block"; return false; }
    std::vector<Pcm> dec; std::vector<MasterInfo> di; std::vector<bool> ok; const std::string de = decodeLaes(body2, core2, base, dec, di, ok); if (!de.empty()) { why = "decode: " + de; return false; }
    for (size_t i = 0; i < ms.size(); i++) if (!ok[i] || !same(dec[i], ms[i].m)) { why = "master " + std::to_string(i + 1) + " differs"; return false; }
    if (coreOut) *coreOut = core; if (infoOut) *infoOut = info; return true; }
static MasterIn M(const Pcm& p) { MasterIn m; m.m = p; return m; }
static MasterIn Mfix(const Pcm& p, int storage, int tier, double T) { MasterIn m; m.m = p; m.storage = storage; m.refTier = tier; setT(m, T); return m; }
// one mix in three word lengths at one rate: 24 bit; 32 bit = 24 bit with a random low byte; 16 bit = the 24 bit TPDF-dithered down
static void sameMix(int rate, double secs, uint64_t seed, Pcm& p32, Pcm& p24, Pcm& p16) {
    p24 = synth(rate, 24, secs, seed); p32 = p24; p32.bits = 32; p16 = p24; p16.bits = 16; Rng r(seed + 99);
    for (int c = 0; c < 2; c++) for (size_t i = 0; i < p24.c[c].size(); i++) { p32.c[c][i] = p24.c[c][i] * 256 + (int64_t)(r.next() & 0xFF);
        p16.c[c][i] = std::max<int64_t>(-32768, std::min<int64_t>(32767, (int64_t)std::floor(p24.c[c][i] / 256.0 + r.uni() + r.uni() + 0.5))); } }

static int run() {
    fails = 0; std::string why;
    // ---- round trips
    report("WAV form, 16-bit core, 24-bit / 352.8 kHz master", roundTrip({M(synth(352800, 24, 0.5, 1))}, {0}, 44100, 16, false, why), why);
    report("FLAC form, automatic 24-bit core, 16 / 88.2 + 24 / 352.8 + 32 / 352.8", roundTrip({M(synth(88200, 16, 0.5, 2)), M(synth(352800, 24, 0.5, 3)), M(synth(352800, 32, 0.5, 4))}, {0, 0, 0}, 44100, 24, true, why), why);
    report("FLAC form, 48 kHz family, 24-bit / 192 kHz", roundTrip({M(synth(192000, 24, 0.5, 5))}, {0}, 48000, 24, true, why), why);
    report("FLAC form, 32-bit core, 32-bit / 176.4 kHz", roundTrip({M(synth(176400, 32, 0.5, 6))}, {0}, 44100, 32, true, why), why);
    report("FLAC form, 16-bit core, 24-bit / 96 kHz", roundTrip({M(synth(96000, 24, 0.5, 7))}, {0}, 48000, 16, true, why), why);
    report("fractional and negative T (+0.37, -1.62 base samples)", roundTrip({M(synth(176400, 24, 0.5, 8)), M(synth(176400, 32, 0.5, 9))}, {0.37, -1.62}, 44100, 16, true, why), why);
    report("unused low bits (22 of 24 bits; FLAC wasted bits)", roundTrip({M(synth(88200, 24, 0.5, 10, 2))}, {0}, 44100, 24, true, why), why);
    report("short master (1500 frames; core shorter than one FLAC block)", roundTrip({M(synth(88200, 24, 1500.0 / 88200, 11))}, {0}, 44100, 24, true, why), why);
    report("odd length (9001 frames at 352.8 kHz)", roundTrip({M(synth(352800, 24, 9001.0 / 352800, 12))}, {0}, 44100, 24, true, why), why);
    report("base-rate master (24-bit / 44.1 kHz, no octave layers)", roundTrip({M(synth(44100, 24, 0.5, 13))}, {0}, 44100, 16, false, why), why);
    {   // storage 2: the same mix in 32 / 24 / 16 bit; the 24 and 16 bit masters must be stored against an earlier master at 176.4 kHz,
        // and cost about what is new in them (16 bit: the dither, about 1 bit per sample; 24 bit from 32: close to nothing)
        Pcm a32, a24, a16; sameMix(176400, 0.5, 30, a32, a24, a16); std::vector<MasterInfo> inf;
        bool ok = roundTrip({M(a32), M(a24), M(a16)}, {0, 0, 0}, 44100, 24, true, why, nullptr, nullptr, &inf);
        if (ok) { const double n = 176400.0 * 0.5 * 2; char b[200]; snprintf(b, sizeof b, "storages %d/%d/%d, tiers %d/%d/%d, bits per sample %.2f / %.2f / %.2f", inf[0].storage, inf[1].storage, inf[2].storage,
                      inf[0].refTier, inf[1].refTier, inf[2].refTier, inf[0].layerBits / n, inf[1].layerBits / n, inf[2].layerBits / n); why = b;
            ok = inf[1].storage == 2 && inf[2].storage == 2 && inf[2].layerBits / n < 2.0; }
        report("storage 2: same mix 32 / 24 / 16 bit at 176.4 kHz, lower words against an earlier master at the full rate", ok, why); printf("      (%s)\n", why.c_str()); }
    {   Pcm a32, a24, a16; sameMix(88200, 0.5, 31, a32, a24, a16);   // storage 2 given by hand, with a fractional T relative to the tier, and storage 0 against a master
        report("storage 2 given by hand, T +0.31 relative to the tier", roundTrip({M(a32), Mfix(a24, 2, 1, 0.31), Mfix(a16, 0, 2, -0.5)}, {0, 0, 0}, 44100, 16, false, why), why); }
    {   Pcm a32, a24, a16; sameMix(352800, 0.25, 32, a32, a24, a16); Pcm b16 = synth(88200, 16, 0.25, 33);   // tier with fewer stages: shared level 88.2 kHz
        report("storage 2 across rates: 16 / 88.2 against a 24 / 352.8 master", roundTrip({M(a24), Mfix(b16, 2, 1, 0)}, {0, 0}, 44100, 24, true, why), why); }
    { const Pcm s = synth(176400, 24, 0.2, 14); const Pcm a = derive16(s), b = derive16(s); bool differs = false; for (size_t i = 0; i < s.c[0].size() && !differs; i++) differs = a.c[0][i] != (s.c[0][i] >> 8);
      report("derived 16-bit: the same on every run, and not a plain truncation", same(a, b) && a.bits == 16 && differs); }

    // ---- damage: a small file with three masters
    std::vector<uint8_t> f; Pcm core; std::vector<MasterIn> ms = {M(synth(88200, 16, 0.25, 20)), M(synth(176400, 24, 0.25, 21)), M(synth(176400, 32, 0.25, 22))};
    if (!roundTrip(ms, {0, 0, 0}, 44100, 24, true, why, &f, &core)) { report("damage tests: base file", false, why); return fails; }
    std::vector<uint8_t> body; { laesflac::Info fi; V c2[2]; bool m5; laesflac::readFlac(f, fi, c2, body, m5); }
    auto fixCrc = [](std::vector<uint8_t>& b) { uint32_t crc = 0xFFFFFFFFu; for (size_t i = 0; i + 4 < b.size(); i++) { crc ^= b[i]; for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; } crc ^= 0xFFFFFFFFu;
        for (int i = 0; i < 4; i++) b[b.size() - 4 + i] = (uint8_t)(crc >> (8 * i)); };
    auto decodes = [&](const std::vector<uint8_t>& b, bool& allOk) { std::vector<Pcm> dec; std::vector<MasterInfo> di; std::vector<bool> ok; const std::string e = decodeLaes(b, core, 44100, dec, di, ok);
        allOk = e.empty() && ok.size() == ms.size(); for (bool o : ok) allOk &= o; if (allOk) for (size_t i = 0; i < ms.size(); i++) allOk &= same(dec[i], ms[i].m); return e.empty(); };
    Rng r(4242);
    { int caught = 0, n = 400; for (int t = 0; t < n; t++) { std::vector<uint8_t> b = body; b[r.next() % b.size()] ^= (uint8_t)(1 + r.next() % 255); bool ok; if (!decodes(b, ok)) caught++; }
      report("LAES block: 400 single-byte flips, all caught by CRC-32", caught == n, std::to_string(caught) + " of " + std::to_string(n)); }
    { int err = 0, shaBad = 0, wrongButOk = 0, intact = 0, n = 300;
      for (int t = 0; t < n; t++) { std::vector<uint8_t> b = body; const int k = 1 + (int)(r.next() % 4); for (int j = 0; j < k; j++) b[r.next() % (b.size() - 4)] ^= (uint8_t)(1 + r.next() % 255); fixCrc(b);
          std::vector<Pcm> dec; std::vector<MasterInfo> di; std::vector<bool> ok; const std::string e = decodeLaes(b, core, 44100, dec, di, ok);
          if (!e.empty()) { err++; continue; } bool allSha = true; for (bool o : ok) allSha &= o; if (!allSha) { shaBad++; continue; }
          bool eq = dec.size() == ms.size(); for (size_t i = 0; eq && i < ms.size(); i++) eq = same(dec[i], ms[i].m); if (eq) intact++; else wrongButOk++; }
      report("LAES block: 300 flips behind a recomputed CRC (field checks / SHA-256), no wrong master reported as good", wrongButOk == 0,
             std::to_string(wrongButOk) + " wrong masters passed SHA-256");
      printf("      (%d rejected by field checks, %d by SHA-256, %d flips in reserved / unused bytes left every master intact)\n", err, shaBad, intact); }
    { int caught = 0, n = 0; for (size_t cut = 0; cut + 1 < body.size(); cut += std::max<size_t>(1, body.size() / 200)) { n++; std::vector<uint8_t> b(body.begin(), body.begin() + cut); bool ok; if (!decodes(b, ok)) caught++; }
      report("LAES block: truncated at 200 points, all rejected", caught == n, std::to_string(caught) + " of " + std::to_string(n)); }
    // FLAC file: flips anywhere, truncation; a read that succeeds with a matching MD5 must give the original core
    { int wrong = 0, n = 400; for (int t = 0; t < n; t++) { std::vector<uint8_t> g = f; g[4 + r.next() % (g.size() - 4)] ^= (uint8_t)(1 + r.next() % 255);
          laesflac::Info fi; V c2[2]; std::vector<uint8_t> app; bool m5; const std::string e = laesflac::readFlac(g, fi, c2, app, m5);
          if (e.empty() && m5 && (c2[0] != core.c[0] || c2[1] != core.c[1])) wrong++; }
      report("FLAC file: 400 single-byte flips, never a wrong core with a matching MD5", wrong == 0, std::to_string(wrong) + " wrong"); }
    { int bad = 0, n = 0; for (size_t cut = 4; cut + 1 < f.size(); cut += std::max<size_t>(1, f.size() / 300)) { n++; std::vector<uint8_t> g(f.begin(), f.begin() + cut);
          laesflac::Info fi; V c2[2]; std::vector<uint8_t> app; bool m5; const std::string e = laesflac::readFlac(g, fi, c2, app, m5); if (e.empty() && m5) bad++; }
      report("FLAC file: truncated at 300 points, none accepted as complete", bad == 0, std::to_string(bad) + " accepted"); }
    { int n = 3000; for (int t = 0; t < n; t++) { std::vector<uint8_t> g(64 + r.next() % 4096); for (auto& x : g) x = (uint8_t)r.next(); const int bps = t % 3 == 0 ? 16 : t % 3 == 1 ? 24 : 32;
          laesflac::BR br{g.data(), g.size()}; std::vector<int64_t> x((size_t)(1 + r.next() % 4096)); laesflac::readSubframe(br, x.data(), (long)x.size(), bps); }
      report("FLAC subframe parser: 3000 random inputs, no crash or hang", true); }
    { int n = 500; for (int t = 0; t < n; t++) { std::vector<uint8_t> g = f; for (size_t i = 42 + (r.next() % 64); i < g.size(); i += 1 + r.next() % 97) g[i] = (uint8_t)r.next();
          laesflac::Info fi; V c2[2]; std::vector<uint8_t> app; bool m5; laesflac::readFlac(g, fi, c2, app, m5); }
      report("FLAC reader: 500 files with scattered garbage, no crash or hang", true); }
    printf("selftest: %s (%d failed)\n", fails ? "FAILED" : "all passed", fails); return fails; }
}  // namespace selftest
