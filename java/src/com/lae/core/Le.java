// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

/** Little-endian reads and writes in byte arrays (all values as the C++ reads them from memory on x64). */
public final class Le {
    private Le() {}

    public static int u8(byte[] b, int o) { return b[o] & 0xFF; }
    public static int u16(byte[] b, int o) { return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8; }
    public static int s16(byte[] b, int o) { return (short) u16(b, o); }
    public static int s24(byte[] b, int o) { return ((b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8 | b[o + 2] << 16); }
    /** uint32 / int32 (same bits) */
    public static int i32(byte[] b, int o) { return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8 | (b[o + 2] & 0xFF) << 16 | b[o + 3] << 24; }
    public static long u32(byte[] b, int o) { return i32(b, o) & 0xFFFFFFFFL; }
    public static long i64(byte[] b, int o) { return (i32(b, o) & 0xFFFFFFFFL) | ((long) i32(b, o + 4) << 32); }

    public static void put16(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }
    public static void put24(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); b[o + 2] = (byte) (v >> 16); }
    public static void put32(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); b[o + 2] = (byte) (v >> 16); b[o + 3] = (byte) (v >> 24); }
    public static void put64(byte[] b, int o, long v) { put32(b, o, (int) v); put32(b, o + 4, (int) (v >>> 32)); }

    /** big-endian (FLAC headers, SHA-256 words) */
    public static int be16(byte[] b, int o) { return (b[o] & 0xFF) << 8 | (b[o + 1] & 0xFF); }
    public static int be24(byte[] b, int o) { return (b[o] & 0xFF) << 16 | (b[o + 1] & 0xFF) << 8 | (b[o + 2] & 0xFF); }
    public static int be32(byte[] b, int o) { return (b[o] & 0xFF) << 24 | (b[o + 1] & 0xFF) << 16 | (b[o + 2] & 0xFF) << 8 | (b[o + 3] & 0xFF); }
}
