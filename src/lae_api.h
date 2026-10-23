/* SPDX-FileCopyrightText: 2026 mtq0605
 * SPDX-License-Identifier: Apache-2.0
 * LAE C interface (liblae): decoding LAE v5 audio in players and plugins.
 * A track is opened from a v5 WAV (CD layer + "LAE " chunk), or from a base layer (the CD layer as PCM, e.g. decoded from FLAC or
 * ripped from a CD) plus its extension file (.lax, spec/cdx/SPEC.md section 3). A base layer is accepted only when its
 * whole-track MD5 matches the extension file (F3), or when the track is found in it by content and then matches (section 6).
 * Then a route and an output rate are chosen and the output is read in order. Output is always 2 channels, interleaved, as int32
 * values: 24-bit range for the routes CD24, 16 and FULL; 16-bit range for the route CD.
 * Threads: one track must not be used by two threads at once; different tracks may be decoded in different threads at the same time.
 * Floating point: the decoder needs strict IEEE-754 double arithmetic in a fixed order (x64/x86 SSE2: MXCSR round-to-nearest, no
 * flush-to-zero, no denormals-are-zero). Hosts whose threads may run with another floating-point state (codecs, drivers or plugins can
 * change it) should set MXCSR to 0x1F80 around every call and restore it afterwards. lae_selftest() checks the calling thread; if it
 * returns 0, play the CD layer only. lae_start() runs the self-test again on its own thread.
 * Static linking: define LAE_STATIC in the host (and when compiling lae_api.cpp) so that no DLL import or export is declared. */
#ifndef LAE_API_H
#define LAE_API_H
#include <stdint.h>
#include <stddef.h>
#if defined(_WIN32) && !defined(LAE_STATIC)
#  ifdef LAE_API_BUILD
#    define LAE_API __declspec(dllexport)
#  else
#    define LAE_API __declspec(dllimport)
#  endif
#  define LAE_CALL __cdecl
#elif defined(_WIN32)
#  define LAE_API
#  define LAE_CALL __cdecl
#else
#  define LAE_API
#  define LAE_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* grows only when functions are added: a host needs lae_api_version() >= the version it was built against
   (2: lae_open_reader; 3: lae_stream_bytes) */
#define LAE_API_VERSION 3
enum lae_route {
    LAE_ROUTE_CD = 0,     /* the CD layer itself (16-bit, base rate) */
    LAE_ROUTE_CD24 = 1,   /* CD layer + depth layer (24-bit, base rate) */
    LAE_ROUTE_16 = 2,     /* CD layer + frequency layer (24-bit, master rate or an octave below) */
    LAE_ROUTE_FULL = 3    /* all layers (24-bit, master rate or an octave below) */
};
typedef struct lae_info {
    int master_rate, base_rate, stages, cutoff_hz;   /* cutoff_hz: CD cut-off (top of the CD band) */
    uint64_t cd_frames, master_frames;               /* lengths of the CD layer and of the master */
    char cd_md5[33];                                 /* whole-track MD5 of the CD layer (lower-case hex) */
    int from_pair;                                   /* 1 when opened from a base layer + extension file */
} lae_info;
typedef struct lae_track lae_track;

LAE_API int LAE_CALL lae_api_version(void);
LAE_API int LAE_CALL lae_selftest(void);   /* 1 when this build and this thread's FPU state decode correctly */
/* paths are UTF-8; err (may be NULL) receives a message on failure */
LAE_API lae_track* LAE_CALL lae_open_file(const char* path, char* err, int errlen);
/* the bytes of a whole v5 WAV file (for hosts that read files themselves, e.g. through archives or streams) */
LAE_API lae_track* LAE_CALL lae_open_memory(const void* wav, size_t len, char* err, int errlen);
/* a v5 WAV file of `size` bytes read through the host: read(ctx, offset, buf, len) fills buf with the len bytes at offset and returns 0,
   or returns -1 (read error, or the host aborts: the open fails). Only the CD layer and the LAE chunk are kept, so the memory needed is
   about the file size (lae_open_file works the same way). Large bodies are read in blocks of at most 4 MiB. */
typedef int (LAE_CALL* lae_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);
LAE_API lae_track* LAE_CALL lae_open_reader(lae_read_fn read, void* ctx, uint64_t size, char* err, int errlen);
LAE_API lae_track* LAE_CALL lae_open_pair(const int16_t* pcm, uint64_t frames, int rate, const void* lax, size_t laxlen, char* err, int errlen);
LAE_API lae_track* LAE_CALL lae_open_pair_files(const char* base_wav, const char* lax_file, char* err, int errlen);
LAE_API int LAE_CALL lae_get_info(const lae_track* t, lae_info* out);
/* out_rate 0 = master rate (routes 16, FULL) or base rate (CD, CD24). Starts at output frame 0. Returns 0, or -1 with err filled;
   after a failure the track is not started (lae_read and lae_seek return -1 until a successful lae_start). */
LAE_API int LAE_CALL lae_start(lae_track* t, int route, int out_rate, char* err, int errlen);
LAE_API int LAE_CALL lae_output_format(const lae_track* t, int* rate, int* bits, uint64_t* frames);
/* next frames in order; returns frames written (0 at the end) or -1 on a stream error */
LAE_API int64_t LAE_CALL lae_read(lae_track* t, int32_t* out, int64_t frames);
/* to an output frame: forward decodes and discards, backward restarts from the beginning. Returns 0 or -1. */
LAE_API int LAE_CALL lae_seek(lae_track* t, uint64_t frame);
/* since lae_start: samples held by the decoder's hard limiter, and whether a stream error occurred (both may be NULL) */
LAE_API int LAE_CALL lae_stats(const lae_track* t, uint64_t* limited, int* stream_error);
/* the read position summed over the entropy-coded extension streams the route decodes (bytes; 0 on the CD route). Two calls with only
   lae_read in between differ by the bytes those reads consumed, so a live bitrate is that difference per second of output plus the CD
   layer's fixed rate; a seek or start may move it back (a checkpoint) and the first read after it may include the catch-up to the
   target, so measure from after that read. Returns 0, or -1. */
LAE_API int LAE_CALL lae_stream_bytes(const lae_track* t, uint64_t* bytes);
LAE_API void LAE_CALL lae_close(lae_track* t);

#ifdef __cplusplus
}
#endif
#endif
