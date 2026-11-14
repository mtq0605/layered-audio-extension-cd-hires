// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// LAE hi-res test suite (generated signals; the generated output is released under CC0-1.0, this program is Apache-2.0). Deterministic: same output on every run.
// usage: gen_suite <out dir> <rate> [seconds = 20] [category]   -> <dir>/<category>_<rate>.wav, 24-bit stereo (all categories, or the one named)
// Categories: levels, sweep, imd, square, transients, electronic, noise, decay, stereo, buried.
// Platform-identical since 2026-11-07: no C-library sin / cos / exp / pow / log, only lmath (lae_math.h: IEEE + - * / sqrt floor
// frexp ldexp in a fixed order); angles in half-turns (sinpi(2 f t) = sin(2 pi f t)), so no rounded pi enters a phase. Compile with
// /fp:strict (MSVC) or -ffp-contract=off (GCC / Clang), like the encoder.
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <cstring>
#include "wavio.h"
#include "lae_math.h"
using namespace std;
static const double PI = 3.14159265358979323846, LN2 = 0.69314718055994530942;
struct Rng { uint32_t s; double u() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s >> 8) / 16777216.0; } double g() { double a = u() + 1e-12, b = u(); return sqrt(-2 * LN2 * lmath::log2(a)) * lmath::cospi(2 * b); } };
static double dB(double d) { return lmath::exp10(d / 20.0); }
static double expE(double x) { return lmath::exp2(x * 1.4426950408889634074); }   // e^x = 2^(x log2 e)
// band-limited oscillators by additive synthesis up to 0.45 x rate; the phase ph in half-turns (2 f / fs per sample)
static double sawBL(double ph, double f, double fs) { double v = 0; for (int k = 1; k * f < 0.45 * fs; k++) v += lmath::sinpi(k * ph) / k; return v * 2 / PI; }
static double sqBL(double ph, double f, double fs) { double v = 0; for (int k = 1; k * f < 0.45 * fs; k += 2) v += lmath::sinpi(k * ph) / k; return v * 4 / PI; }
// Speed (2026-11-12): the band-limited oscillators sum up to thousands of lmath::sinpi per sample (55 Hz saw at 352.8 kHz: 2886
// harmonics), which made one 20-s file take 11 minutes. Their value at sample i depends only on that sample's phase and frequency, so
// the phases are accumulated in order as before and the oscillator values are then computed on all cores by the same function: every
// value, and so every output byte, is the same as with one thread. body(a, b) handles samples [a, b).
static void parallelFor(long n, const function<void(long, long)>& body) {
    unsigned nt = thread::hardware_concurrency(); if (nt == 0) nt = 1; if (nt > 64) nt = 64; if (n < 65536) nt = 1;
    vector<thread> th; long chunk = (n + nt - 1) / nt;
    for (unsigned k = 0; k < nt; k++) { long a = (long)k * chunk, b = min(n, a + chunk); if (a >= b) break; th.emplace_back(body, a, b); }
    for (auto& x : th) x.join(); }
struct Biquad { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0; double run(double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
static Biquad hp(double fc, double fs) { double w = 2 * fc / fs, c = lmath::cospi(w), s = lmath::sinpi(w) / (2 * 0.7071), a0 = 1 + s; return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - s) / a0}; }
static Biquad lp(double fc, double fs) { double w = 2 * fc / fs, c = lmath::cospi(w), s = lmath::sinpi(w) / (2 * 0.7071), a0 = 1 + s; return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - s) / a0}; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: gen_suite <out dir> <rate> [seconds = 20] [category]\n"); return 1; }
    string dir = argv[1]; int fs = atoi(argv[2]); double secs = argc > 3 ? atof(argv[3]) : 20;
    if (fs < 4 || !(secs >= 0) || secs * fs > 2147483647.0) { fprintf(stderr, "gen_suite: the rate must be at least 4 Hz and the length not negative (at most 2^31 - 1 frames)\n"); return 1; }
    long n = (long)(secs * fs); const double FS = 8388607.0;
    // optional category: only that file is generated (the categories are independent: each writes all of L and R from its own state)
    static const char* cats[] = {"levels", "sweep", "imd", "square", "transients", "electronic", "noise", "decay", "stereo", "buried"};
    const char* only = argc > 4 ? argv[4] : nullptr;
    if (only) { bool known = false; for (const char* c : cats) known = known || !strcmp(c, only);
        if (!known) { fprintf(stderr, "gen_suite: unknown category '%s'\n", only); return 1; } }
    auto want = [&](const char* c) { return !only || !strcmp(c, only); };
    auto save = [&](const string& name, const vector<double>& L, const vector<double>& R) { vector<int32_t> s(n * 2); double pk = 0;
        for (long i = 0; i < n; i++) { pk = max(pk, max(fabs(L[i]), fabs(R[i]))); }
        double g = pk > 1.0 ? dB(-0.5) / pk : 1.0;   // never clip the test material: a clip that would exceed full scale is scaled to -0.5 dBFS peak
        for (long i = 0; i < n; i++) { s[i * 2] = (int32_t)lround(L[i] * g * FS); s[i * 2 + 1] = (int32_t)lround(R[i] * g * FS); } pk *= g;
        string p = (std::filesystem::path(dir) / (name + "_" + to_string(fs) + ".wav")).string(); if (!wavio::write24(p.c_str(), fs, s)) { fprintf(stderr, "gen_suite: cannot write %s\n", p.c_str()); exit(2); } printf("%-40s peak %.2f dBFS\n", p.c_str(), 20 * lmath::log10(pk + 1e-12)); };
    vector<double> L(n), R(n);
    // levels: 1 kHz at 0, -20, -60, -100, -120 dBFS (2 s each), then 30 kHz and 60 kHz tones at -40 dBFS, then silence with TPDF dither
    if (want("levels")) { Rng r{7}; for (long i = 0; i < n; i++) { double t = (double)i / fs, v = 0; int seg = (int)(t / 2);
        double lv[5] = {-0.1, -20, -60, -100, -120};
        if (seg < 5) v = dB(lv[seg]) * lmath::sinpi(2 * 1000 * t);
        else if (seg < 7) v = dB(-40) * lmath::sinpi(2 * (seg == 5 ? 30000 : 60000) * t);
        else v = (r.u() - r.u()) / FS;
        L[i] = R[i] = v; } save("levels", L, R); }
    // sweep: logarithmic 10 Hz -> 0.45 fs at -6 dBFS
    if (want("sweep")) { double ph = 0; for (long i = 0; i < n; i++) { double t = (double)i / fs, f = 10 * lmath::exp2(lmath::log2(0.45 * fs / 10) * (t / secs)); ph += 2 * f / fs; L[i] = R[i] = dB(-6) * lmath::sinpi(ph); } save("sweep", L, R); }
    // imd: SMPTE 60 Hz + 7 kHz (4:1), CCIF 19 + 20 kHz, ultrasonic 40 + 41 kHz (each a third of the clip)
    if (want("imd")) { for (long i = 0; i < n; i++) { double t = (double)i / fs; int seg = (int)(3 * t / secs); double v;
        if (seg == 0) v = 0.8 * lmath::sinpi(2 * 60 * t) * 0.8 + 0.2 * 0.8 * lmath::sinpi(2 * 7000 * t);
        else if (seg == 1) v = 0.45 * (lmath::sinpi(2 * 19000 * t) + lmath::sinpi(2 * 20000 * t));
        else v = 0.45 * (lmath::sinpi(2 * 40000 * t) + lmath::sinpi(2 * 41000 * t));
        L[i] = R[i] = v; } save("imd", L, R); }
    // square / saw: band-limited, near full scale (inter-sample peaks), 110 Hz, 997 Hz, 5 kHz
    // (the phase is accumulated in order; the oscillator values are computed in parallel from the stored phases, see parallelFor)
    if (want("square")) { auto segOf = [&](long i) { double t = (double)i / fs; return (int)(6 * t / secs); };
        auto fOf = [&](int seg) -> double { return seg % 3 == 0 ? 110 : seg % 3 == 1 ? 997 : 5000; };
        vector<double> phs(n); double ph = 0; for (long i = 0; i < n; i++) { double f = fOf(segOf(i)); ph += 2 * f / fs; phs[i] = ph; }
        parallelFor(n, [&](long a, long b) { for (long i = a; i < b; i++) { int seg = segOf(i); double f = fOf(seg);
            double v = seg < 3 ? 0.9 * sqBL(phs[i], f, fs) : 0.9 * sawBL(phs[i], f, fs); L[i] = R[i] = v; } });
        save("square", L, R); }
    // transients: Dirac pulses, synthetic kick / snare / hi-hat with ultrasonic content, 120 BPM
    if (want("transients")) { Rng r{11}; Biquad hh = hp(8000, fs), hh2 = hp(8000, fs); for (long i = 0; i < n; i++) L[i] = R[i] = 0;
        for (long i = 0; i < n; i++) { double t = (double)i / fs, beat = fmod(t, 0.5), bar = fmod(t, 2.0); double v = 0;
            if (t < 2) { if (i % (fs / 4) == 0) v = 0.95; }                                                                  // Dirac pulses
            else { if (fmod(bar, 1.0) < 0.3) { double tt = fmod(bar, 1.0); v += 0.8 * expE(-tt * 18) * lmath::sinpi(2 * (45 + 120 * expE(-tt * 30)) * tt); }   // kick
                if (bar >= 0.5 && bar < 0.7 || bar >= 1.5 && bar < 1.7) { double tt = fmod(bar - 0.5, 1.0); v += 0.5 * expE(-tt * 25) * (0.6 * r.g() * 0.4 + 0.4 * lmath::sinpi(2 * 190 * tt)); }   // snare
                double th = fmod(t, 0.125); v += 0.3 * expE(-th * 60) * hh.run(r.g() * 0.5); }                             // hi-hat (full band)
            L[i] = v; R[i] = v * 0.9; } save("transients", L, R); }
    // electronic: saw bass, detuned saw pads, square arpeggio, hats and claps, side-chain pumping, ultrasonic shimmer
    if (want("electronic")) { Rng r{23}; Biquad bl = lp(900, fs), pl = lp(6000, fs), pr = lp(6000, fs), hh = hp(9000, fs);
        double notes[8] = {55, 55, 65.41, 73.42, 55, 55, 82.41, 73.42}; double chord[3] = {1.0, 1.2599, 1.4983};
        // bass saw and arpeggio square: phases accumulated in order, then the oscillator values in parallel (see parallelFor);
        // stepOf / fbOf / rootOf / faOf are the expressions of the sample loop, used by both passes
        auto stepOf = [&](long i) { double t = (double)i / fs; return (int)(t * 8); };
        auto fbOf = [&](int step) { return notes[(step / 2) % 8]; };
        auto rootOf = [&](int step) { return 220 * (((step / 32) % 2) ? 1.122 : 1.0); };
        auto faOf = [&](int step) { return rootOf(step) * 4 * chord[step % 3]; };
        vector<double> sawv(n), sqv(n); { double pb = 0, pa = 0; for (long i = 0; i < n; i++) { int step = stepOf(i);
            pb += 2 * fbOf(step) / fs; sawv[i] = pb; pa += 2 * faOf(step) / fs; sqv[i] = pa; } }
        parallelFor(n, [&](long a, long b) { for (long i = a; i < b; i++) { int step = stepOf(i);
            sawv[i] = sawBL(sawv[i], fbOf(step), fs); sqv[i] = sqBL(sqv[i], faOf(step), fs); } });   // phase -> oscillator value, in place
        for (long i = 0; i < n; i++) { double t = (double)i / fs; int step = (int)(t * 8); double beat = fmod(t, 0.5);
            double duck = 1 - 0.7 * expE(-beat * 12);                                                                         // side-chain
            double bass = bl.run(sawv[i]) * 0.5;
            double root = rootOf(step); double padL = 0, padR = 0;
            for (int c = 0; c < 3; c++) { double f = root * chord[c]; padL += lmath::sinpi(2 * f * 1.003 * t) + 0.5 * lmath::sinpi(4 * f * 1.003 * t); padR += lmath::sinpi(2 * f * 0.997 * t) + 0.5 * lmath::sinpi(4 * f * 0.997 * t); }
            double arp = 0.25 * sqv[i] * expE(-fmod(t, 0.125) * 20);
            double hat = 0.25 * hh.run(r.g()) * expE(-fmod(t + 0.0625, 0.125) * 70);
            double clap = (fmod(t, 1.0) > 0.5 && fmod(t, 1.0) < 0.6) ? 0.4 * r.g() * expE(-(fmod(t, 1.0) - 0.5) * 40) : 0;
            double shimmer = fs > 80000 ? 0.05 * lmath::sinpi(2 * 28000 * t) * (0.5 + 0.5 * lmath::sinpi(2 * 0.25 * t)) : 0;
            double kick = beat < 0.3 ? 0.9 * expE(-beat * 16) * lmath::sinpi(2 * (42 + 100 * expE(-beat * 35)) * beat) : 0;
            L[i] = 0.9 * (kick + duck * (bass + 0.12 * pl.run(padL) + arp + hat + clap + shimmer));
            R[i] = 0.9 * (kick + duck * (bass + 0.12 * pr.run(padR) + arp * 0.8 + hat * 1.1 + clap + shimmer)); } save("electronic", L, R); }
    // noise: white, pink (Voss-McCartney style sum), brown at -20 dBFS, then white at -90 dBFS
    if (want("noise")) { Rng r{31}; double br = 0, pk[8] = {0}; for (long i = 0; i < n; i++) { double t = (double)i / fs; int seg = (int)(4 * t / secs); double v = 0, w = r.g();
        if (seg == 0) v = dB(-20) * w;
        else if (seg == 1) { for (int k = 0; k < 8; k++) if ((i & ((1L << k) - 1)) == 0) pk[k] = r.g(); double s = w; for (int k = 0; k < 8; k++) s += pk[k]; v = dB(-20) * s / 3; }
        else if (seg == 2) { br = 0.999 * br + w * 0.03; v = dB(-20) * br; }
        else v = dB(-90) * w;
        L[i] = v; R[i] = dB(-20) * r.g() * (seg < 3 ? 1 : dB(-70)); } save("noise", L, R); }
    // decay: piano-like additive tones decaying into -120 dBFS, then a reverb-like noise tail
    if (want("decay")) { Rng r{41}; Biquad tl = lp(12000, fs); for (long i = 0; i < n; i++) { double t = (double)i / fs; double v = 0; double tn = fmod(t, 5.0); double f0 = t < 5 ? 261.63 : t < 10 ? 110 : t < 15 ? 1046.5 : 0;
            if (f0 > 0) for (int k = 1; k < 30 && k * f0 < 0.45 * fs; k++) v += 0.5 / k * expE(-tn * (1.2 + 0.4 * k)) * lmath::sinpi(2 * k * f0 * (1 + 0.0004 * k * k) * t);
            else v = 0.3 * tl.run(r.g()) * expE(-tn * 2.8);
            L[i] = v; R[i] = v * 0.97; } save("decay", L, R); }
    // stereo: left only, right only, opposite polarity, decorrelated wide noise
    if (want("stereo")) { Rng r{53}; for (long i = 0; i < n; i++) { double t = (double)i / fs; int seg = (int)(4 * t / secs); double s = 0.5 * lmath::sinpi(2 * 440 * t);
        if (seg == 0) { L[i] = s; R[i] = 0; } else if (seg == 1) { L[i] = 0; R[i] = s; } else if (seg == 2) { L[i] = s; R[i] = -s; } else { L[i] = dB(-12) * r.g(); R[i] = dB(-12) * r.g(); } } save("stereo", L, R); }
    // buried: tones from -10 dB to +6 dB relative to a -60 dBFS white noise floor, at 3, 17, 35 kHz (where the rate allows)
    if (want("buried")) { Rng r{67}; for (long i = 0; i < n; i++) { double t = (double)i / fs; double v = dB(-60) * r.g() * 0.25; int seg = (int)(4 * t / secs); double rel[4] = {-10, -6, 0, 6};
        double fl = dB(-60) * 0.25 * sqrt(2.0 / (fs / 2.0) * 50);   // tone level relative to the noise power in ~50 Hz
        for (double f : {3000.0, 17000.0, 35000.0}) if (f < 0.45 * fs) v += fl * dB(rel[seg]) * lmath::sinpi(2 * f * t);
        L[i] = R[i] = v; } save("buried", L, R); }
    return 0; }
