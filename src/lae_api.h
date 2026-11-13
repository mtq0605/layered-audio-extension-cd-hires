/* SPDX-FileCopyrightText: 2026 mtq0605
 * SPDX-License-Identifier: Apache-2.0
 * LAE C interface (liblae): decoding LAE v5 audio in players and plugins.
 * A track is opened from a v5 file - WAV form (CD layer + "LAE " chunk) or FLAC form (CD layer in the FLAC audio frames + "LAE1"
 * APPLICATION blocks; the same open functions, told apart by content, 2026-11-10) -, or from a base layer (the CD layer as PCM, e.g. decoded from FLAC or
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
   (2: lae_open_reader; 3: lae_stream_bytes; 4: the laes_* functions, LAE Lossless; 5: laes_index_save / load;
    6: lae_probe_* and the lae_any_* functions, one interface for LAEY and LAES; 7: laes_master_name, laes_master_flags, laes_choice_from, laes_master_number;
    8: LAEY extension packs: lae_pack_count, lae_pack_info, lae_select_pack, lae_selected_pack, lae_any_choice_pack;
    9: laes_default_choice, laes_core_flags) */
#define LAE_API_VERSION 9
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
/* API 8, LAEY extension packs (spec/lossy/SPEC.md, Section 5.6): one file = one CD / core layer + up to 9 mutually independent extensions,
   each made from its own master. Pack 0 is the main body (what a decoder without pack support plays); packs 1 .. are further ones, each
   with its own master rate and an optional name of the publisher (UTF-8, shown as it is; it never selects anything).
   lae_pack_count: the number of packs, pack 0 included (1 = an ordinary file). lae_pack_info: pack k's master rate, octave count and
   name (pack 0: the name of its optional "LNAM" record, else ""). lae_select_pack: the pack the next lae_start decodes; the track is unstarted afterwards and lae_get_info
   describes that pack (master_rate, stages, cutoff_hz, master_frames). The first selection of a further pack checks its entropy
   streams to their end (as the open does for pack 0). A pack is chosen by the user only, by number: never automatically, never by a
   name or a rate, and the choice is not written into the file. All return -1 for a bad track or pack number. */
LAE_API int LAE_CALL lae_pack_count(const lae_track* t);
LAE_API int LAE_CALL lae_pack_info(const lae_track* t, int k, int* rate, int* stages, char* name, size_t name_size);
LAE_API int LAE_CALL lae_select_pack(lae_track* t, int k);
LAE_API int LAE_CALL lae_selected_pack(const lae_track* t);
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
/* API 7 (2026-11-11, LAES payload version 2): the name of master m (0-based, as laes_choice_info.master): the publisher's UTF-8 text, "" when
   it has none (or for a version 1 file); shown as it is, never used to choose anything. Flags: bit 0 (LAES_MASTER_NO_DERIVE) = the file
   turns its derived 16-bit versions off (they are then not in the list). -1 / NULL for a bad index. laes_choice_from: for a derived
   16-bit choice the index of the choice it is made from (core, master or rate level), else -1. Menus: SPEC 10.3 (the core master and
   the masters on the first level; a master's rate levels and derived versions in its own submenu; the number always shown). */
#define LAES_MASTER_NO_DERIVE 1
LAE_API const char* LAE_CALL laes_master_name(const laes_file* f, int master);
LAE_API int LAE_CALL laes_master_flags(const laes_file* f, int master);
LAE_API int LAE_CALL laes_choice_from(const laes_file* f, int index);
/* API 7: the number of master m (0-based, as laes_choice_info.master) in menus: its place in the order the publisher marked (payload
   version 2), else m + 1. The choice list is already in that order; the file order is the encoder's. 0 for a bad index. */
LAE_API int LAE_CALL laes_master_number(const laes_file* f, int master);
/* API 9 (2026-11-12): the choice a player plays when the user has chosen nothing: the default master written when the file was encoded
   (the master itself), else the core master (0). A player that cannot play it (it does not decode, its SHA-256 differs) plays the core
   master. Never chosen by bit rate, size or device state (SPEC 10.3). laes_core_flags: bit 0 (LAES_MASTER_NO_DERIVE) = the core master
   offers no derived 16-bit version; 0 for a file without the flag or a payload that failed a check. */
LAE_API int LAE_CALL laes_default_choice(const laes_file* f);
LAE_API int LAE_CALL laes_core_flags(const laes_file* f);
/* starts decoding one choice from its first frame; threads 0 = half the logical cores. NULL with err on failure. f must outlive it.
   The file is read through the callback given to laes_open_reader (same ctx): keep it valid, and use one thread per laes_file. */
LAE_API laes_play* LAE_CALL laes_start(laes_file* f, int choice, int threads, char* err, int errlen);
/* as laes_start, and every master of the file is decoded and checked in the same run (one pass; tiers shared): after the last frame,
   laes_master_ok gives the result of each master. For a player's first check of a file. */
LAE_API laes_play* LAE_CALL laes_start_all(laes_file* f, int choice, int threads, char* err, int errlen);
/* starts a choice at output frame `frame` (seeking / switching inside a track). The layers begin at the block positions noted earlier in
   this process (while layers were decoded, or by laes_index_layers); where none is noted the layer is decoded from its start, which is
   correct but slower. No SHA-256 is computed (laes_verified stays 0): only for a master already verified for this file content. A
   derived 16-bit version starts there too (since 2026-11-12): its dither state is moved to the frame, the output is bit for bit the
   same as when decoded from frame 0. The file holds no index; the noted positions are kept in memory only. */
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

/* ---- One interface for both formats, API version 6 (2026-11-10). Implemented in lae_any.cpp, on top of the lae_* and laes_* functions
   above only (link it together with lae_api.cpp and laes_api.cpp; nothing in the two decoders changes).
   Which format a file is, is decided by its content alone, never by its name: an LAEY file and an LAES file may both be called .lae,
   .wav or .flac. The container (RIFF/WAVE, RF64 / BW64, or FLAC) is found from the first bytes; the format from the first 4 bytes of
   the extension: the "LAE " chunk of a WAV file, or the ID of the APPLICATION blocks of a FLAC file. "LAE1" = LAEY (lossy tier,
   container v5), "LAE2" = LAES (lossless). A FLAC file holding both IDs is read as LAES. A file without either is a plain WAV / FLAC. */
#define LAE_FORMAT_NONE 0        /* no LAE extension (a plain WAV / FLAC file, or neither) */
#define LAE_FORMAT_LAEY 1        /* extension LAE1 */
#define LAE_FORMAT_LAES 2        /* extension LAE2 */
#define LAE_CONTAINER_NONE 0     /* neither WAV nor FLAC */
#define LAE_CONTAINER_WAV 1      /* RIFF/WAVE, RF64 or BW64 */
#define LAE_CONTAINER_FLAC 2
typedef struct lae_probe_info { int format, container; char magic[5]; /* "LAE1", "LAE2" or "" */ } lae_probe_info;
/* reads only the structure (milliseconds; no audio, no check). Returns 0 with *out filled (format NONE for a plain file), or -1 with
   err when the file cannot be read or its structure is damaged (out->container is still filled when the first bytes were read). */
LAE_API int LAE_CALL lae_probe_reader(lae_read_fn read, void* ctx, uint64_t size, lae_probe_info* out, char* err, int errlen);
LAE_API int LAE_CALL lae_probe_file(const char* path, lae_probe_info* out, char* err, int errlen);
LAE_API const char* LAE_CALL lae_format_name(int format);   /* "LAEY", "LAES" or "" */

/* An opened file of either format. A choice is one thing the file can play:
     LAEY: the CD layer, CD layer + depth layer, and the routes 16 and FULL at the master rate and at every octave below it down to
           twice the base rate (names cd, cd24, 16_<rate>, full_<rate>);
     LAES: the core master, every master, every rate level of a master, the derived 16-bit versions (the choices of laes_get_choice,
           same order, same names).
   Choice 0 is always the audio a player without LAE support reads from the file (LAEY: the CD layer; LAES: the core master).
   The checks differ per format and are run by the format's own rules; lae_any_get_check lists them in the order they are made:
     LAEY at open:  CRC-32 of the LAE1 block; version and header fields; the CD layer's length against the header; FLAC form: the CD
                    layer's MD5 against STREAMINFO; every entropy stream decoded to its last byte. A file that fails one does not open
                    (err says which; the CD audio itself still plays in any player).
          at the end of a choice: no stream value outside its range.
     LAES at open:  CRC-32 and structure of the LAE2 payload; the core master's SHA-256 against the payload; FLAC form: the core
                    master's MD5 and sample count against STREAMINFO. A payload that fails still opens: only the core master is offered.
          at the end of a choice: the SHA-256 of every master the choice needs (with LAE_ANY_VERIFY_ALL: of every master of the file).
   A check's result: 1 passed, 0 failed, -1 not made yet (it is made by reading a choice to its last frame). */
#define LAE_ANY_VERIFY_ALL 1     /* lae_any_start: check every master of an LAES file in the same run (LAEY: no effect) */
typedef struct lae_any lae_any;
typedef struct lae_any_info {
    int format, container;        /* LAE_FORMAT_*, LAE_CONTAINER_* */
    int version;                  /* LAEY: the container version (5); LAES: 0 */
    int base_rate, base_bits;     /* of choice 0 */
    int choices, default_choice;  /* default: LAEY the route FULL at the master rate; LAES the entry the file names as its default (a master; not set, or the payload failed: the core master) */
    int checks;
    int usable;                   /* 1: every check made at open passed; 0: only choice 0 is offered (reason in the failed check) */
    const char* identity;         /* what names this content: LAEY "CD layer MD5 <hex>", LAES "file SHA-256 <hex>"; lives as long as the handle */
} lae_any_info;
typedef struct lae_any_choice {
    int route;                    /* LAEY: enum lae_route; LAES: -1 */
    int laes_kind;                /* LAES: LAES_KIND_*; LAEY: -1 */
    int rate, bits; uint64_t frames;
    const char* name;             /* short, fit for a file name */
    const char* what;             /* one line for a list */
} lae_any_choice;
typedef struct lae_any_check { const char* method; const char* object; int result; const char* detail; } lae_any_check;
/* paths are UTF-8. NULL with err when the file is neither format ("no LAE extension: ..."), cannot be read, or is an LAEY file that
   fails a check. format (may be NULL) receives what the content was recognised as even then, so that a host can say
   "LAEY file, extension unusable" and play the plain audio. The reader given to lae_any_open_reader must stay valid until lae_any_close. */
LAE_API lae_any* LAE_CALL lae_any_open_file(const char* path, lae_probe_info* format, char* err, int errlen);
LAE_API lae_any* LAE_CALL lae_any_open_reader(lae_read_fn read, void* ctx, uint64_t size, lae_probe_info* format, char* err, int errlen);
LAE_API int LAE_CALL lae_any_get_info(const lae_any* a, lae_any_info* out);
LAE_API int LAE_CALL lae_any_get_choice(const lae_any* a, int index, lae_any_choice* out);   /* the strings live as long as a */
/* the strings live until the next lae_any_start / lae_any_read / lae_any_close on a */
LAE_API int LAE_CALL lae_any_get_check(const lae_any* a, int index, lae_any_check* out);
/* the choice with this name; LAEY also: "16" and "full" (at the master rate). Returns the index, or -1. */
LAE_API int LAE_CALL lae_any_find_choice(const lae_any* a, const char* name);
/* API 8: the extension pack a LAEY choice belongs to (0 = the main body; the choices of pack k are named "pack<k>_..."); -1 for an LAES
   choice or a bad index. In a menu the packs are the first level (number and name), each pack's choices below it. */
LAE_API int LAE_CALL lae_any_choice_pack(const lae_any* a, int index);
/* starts a choice at its first frame. flags: 0 or LAE_ANY_VERIFY_ALL; threads 0 = the format's default. Returns 0, or -1 with err. */
LAE_API int LAE_CALL lae_any_start(lae_any* a, int choice, int flags, int threads, char* err, int errlen);
LAE_API int LAE_CALL lae_any_output_format(const lae_any* a, int* rate, int* bits, uint64_t* frames);
/* next frames in order, interleaved L R, int32 in the choice's word length; returns frames written, 0 at the end (then the checks made
   at the end have their results), -1 on an error (reason: lae_any_error) */
LAE_API int64_t LAE_CALL lae_any_read(lae_any* a, int32_t* out, int64_t frames);
/* to an output frame of the started choice. LAEY: lae_seek (bit-exact). LAES: laes_start_at (no SHA-256 is computed after a seek: the
   checks at the end stay "not made"; a derived 16-bit version starts at the frame like any other choice, bit-exact). Returns 0 or -1. */
LAE_API int LAE_CALL lae_any_seek(lae_any* a, uint64_t frame);
LAE_API const char* LAE_CALL lae_any_error(const lae_any* a);   /* "" when none */
/* 1: the started choice was read to its last frame and every check passed; 0: not at the end yet, or a seek skipped the checks at
   the end; -1: a check failed or an error stopped the decoding */
LAE_API int LAE_CALL lae_any_verified(const lae_any* a);
LAE_API void LAE_CALL lae_any_close(lae_any* a);

#ifdef __cplusplus
}
#endif
#endif
