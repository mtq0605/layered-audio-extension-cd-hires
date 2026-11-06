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
   (2: lae_open_reader; 3: lae_stream_bytes; 4: the laes_* functions, LAE Lossless) */
#define LAE_API_VERSION 5
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
/* to an output frame, bit-exact (the samples a decode from the start gives there). The entropy streams continue from the nearest
   checkpoint, in parallel. Checkpoints come from decoding (in memory, about every 4 s) and from the check every lae_open_* runs the first
   time it meets a file: the SHA-256 of the whole file, every entropy stream decoded to its end (a stream that does not end exactly at its
   last byte fails the open, as a damaged chunk does), a checkpoint every 30 s. These are kept in the player's own folder under that SHA-256
   (%APPDATA%\LAE\lae_index\<sha256>.idx on Windows, $XDG_CACHE_HOME/lae/lae_index or ~/.cache/lae/lae_index elsewhere; the environment
   variable LAE_INDEX_DIR names another folder, "off" none), at most 256 MB (least recently used records deleted first), never in the
   media file. A missing or damaged record only makes the next open check again. Returns 0 or -1. */
LAE_API int LAE_CALL lae_seek(lae_track* t, uint64_t frame);
/* since lae_start: samples held by the decoder's hard limiter, and whether a stream error occurred (both may be NULL) */
LAE_API int LAE_CALL lae_stats(const lae_track* t, uint64_t* limited, int* stream_error);
/* the read position summed over the entropy-coded extension streams the route decodes (bytes; 0 on the CD route). Two calls with only
   lae_read in between differ by the bytes those reads consumed, so a live bitrate is that difference per second of output plus the CD
   layer's fixed rate; a seek or start may move it back (a checkpoint) and the first read after it may include the catch-up to the
   target, so measure from after that read. Returns 0, or -1. */
LAE_API int LAE_CALL lae_stream_bytes(const lae_track* t, uint64_t* bytes);
LAE_API void LAE_CALL lae_close(lae_track* t);

/* ---- LAE Lossless (LAES, extension LAE2), API version 4 (2026-11-01). Implemented in laes_api.cpp (compile it with strict
   floating point: MSVC /fp:strict, GCC / Clang -ffp-contract=off). A WAV file whose "LAE " chunk begins with LAE2, or a FLAC file with
   APPLICATION blocks "LAE2". A choice is the core, a master, a rate level of a master, or a derived 16-bit master (lossless relative to its definition, not relative to its source). The file
   has no index (spec, rules T-1 / T-2): a choice is decoded in time order from its start. A master's SHA-256 is known only after its
   last sample: a player presents a master as that master only after a run of laes_read to the end gave laes_verified() == 1 for the
   same file content (spec 1.3); it may remember that by file_sha256 and the choice's name. */
#define LAES_KIND_CORE 0
#define LAES_KIND_MASTER 1
#define LAES_KIND_LEVEL 2
#define LAES_KIND_DERIVED16 3
typedef struct laes_file laes_file;
typedef struct laes_play laes_play;
typedef struct laes_file_info {
    int is_flac, base_rate, core_bits, choices;
    uint64_t core_frames;
    int payload_ok;               /* 0: the payload failed a check (reason in laes_file_problem); only the core plays */
    unsigned char file_sha256[32];/* SHA-256 of every byte of the file (all zero after a quick open) */
    int quick;                    /* 1: opened for track info only (laes_start refuses) */
} laes_file_info;
typedef struct laes_choice_info { int kind, master, level, rate, bits; uint64_t frames; const char* name; const char* what; } laes_choice_info;
/* opens an LAES file read through the host. quick_info = 1 reads only the structure (milliseconds, for track info); 0 reads the whole
   file once (its SHA-256, the payload CRC-32, the core and its hash). Returns NULL with err "not LAES" for a file without an LAE2
   payload (lae_magic, if not NULL, receives "LAE1" for an LAEY file), or another reason. */
LAE_API laes_file* LAE_CALL laes_open_reader(lae_read_fn read, void* ctx, uint64_t size, int quick_info, char lae_magic[5], char* err, int errlen);
LAE_API int LAE_CALL laes_get_info(const laes_file* f, laes_file_info* out);
LAE_API const char* LAE_CALL laes_file_problem(const laes_file* f);   /* "" when the payload passed every check */
LAE_API int LAE_CALL laes_get_choice(const laes_file* f, int index, laes_choice_info* out);   /* the strings live as long as f */
/* tags: FLAC VORBIS_COMMENT entries, or WAV LIST/INFO entries (4-letter keys); index from 0; returns -1 past the last */
LAE_API int LAE_CALL laes_get_tag(const laes_file* f, int index, const char** key, const char** value);
/* starts decoding one choice from its first frame; threads 0 = half the logical cores. NULL with err on failure. f must outlive it.
   The file is read through the callback given to laes_open_reader (same ctx): keep it valid, and use one thread per laes_file. */
LAE_API laes_play* LAE_CALL laes_start(laes_file* f, int choice, int threads, char* err, int errlen);
/* as laes_start, and every master of the file is decoded and checked in the same run (one pass; tiers shared): after the last frame,
   laes_master_ok gives the result of each master. For a player's first check of a file. */
LAE_API laes_play* LAE_CALL laes_start_all(laes_file* f, int choice, int threads, char* err, int errlen);
/* starts a choice at output frame `frame` (seeking / switching inside a track). The layers begin at the block positions noted earlier in
   this process (while layers were decoded, or by laes_index_layers); where none is noted the layer is decoded from its start, which is
   correct but slower. No SHA-256 is computed (laes_verified stays 0): only for a master already verified for this file content. Not
   for derived 16-bit versions (their dither runs from frame 0). The file holds no index; the noted positions are kept in memory only. */
LAE_API laes_play* LAE_CALL laes_start_at(laes_file* f, int choice, uint64_t frame, int threads, char* err, int errlen);
/* decodes every layer of the file once, entropy decoding only, noting where each block begins (memory only), so that laes_start_at can
   begin near any frame. Seconds for a 10-minute file; a host runs it on a background thread. Stops early when *cancel becomes non-zero. */
LAE_API void LAE_CALL laes_index_layers(laes_file* f, const volatile int* cancel, int threads);
/* the player's own record of block positions, for its profile folder (never written into the media file): every 16th block start of
   every layer, with the file's SHA-256 and a CRC-32. laes_index_save returns the record's size (0 while laes_index_layers has not
   noted every layer yet) and writes it when cap is large enough. laes_index_load returns 0 when the record matches this file (it then
   serves laes_start_at at once and laes_index_layers has nothing left to do), -1 otherwise (nothing is changed). API version 5. */
LAE_API int64_t LAE_CALL laes_index_save(laes_file* f, void* buf, int64_t cap);
LAE_API int LAE_CALL laes_index_load(laes_file* f, const void* data, int64_t len);
/* next frames in order, interleaved L R, int32 in the choice's word length; returns frames written, 0 at the end, -1 on an error */
LAE_API int64_t LAE_CALL laes_read(laes_play* p, int32_t* out, int64_t frames);
/* 1: the last frame was read and every master the choice depends on matched its SHA-256 (always 1 for the core); 0: not yet at the
   end, or a mismatch (then laes_play_error says so); -1: an error stopped the decoding */
LAE_API int LAE_CALL laes_verified(const laes_play* p);
LAE_API const char* LAE_CALL laes_play_error(const laes_play* p);
/* after laes_verified() != 0 for a finished run: whether master m (0-based) was decoded in this run and matched its SHA-256: 1 yes, 0 mismatch,
   -1 not part of this run (a choice decodes its master and the masters it refers to). */
LAE_API int LAE_CALL laes_master_ok(const laes_play* p, int m);
LAE_API void LAE_CALL laes_stop(laes_play* p);
LAE_API void LAE_CALL laes_close(laes_file* f);

#ifdef __cplusplus
}
#endif
#endif
