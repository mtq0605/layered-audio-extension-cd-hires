// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// ISO 9660 Level 1 + Joliet for the data session of an LAE CD-Extra disc (spec/cdx/SPEC.md 2.3, D1-D7):
// a deterministic volume writer and a minimal reader. Written from ECMA-119 4th ed. and the Joliet specification; no third-party code.
// The volume is plain 2048-byte sectors (no raw sectors, EDC or ECC). Its sector 0 lies at absolute disc LBA startLba, and every address
// recorded in it (path tables, directory and file extents) and the volume space size (startLba + sectors) are absolute.
// Layout in sectors relative to startLba: 0-15 system area (zeros), 16 primary descriptor, 17 Joliet supplementary descriptor (UCS-2
// Level 3), 18 set terminator, path tables (primary L, M, Joliet L, M), primary then Joliet directories in path-table order, file extents
// (one copy shared by both trees; directories in path-table order, files sorted within each), tail zero sectors.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
namespace iso {
struct File { std::string path; std::vector<uint8_t> data; };              // '/'-separated: "README.TXT", "LAE/TRACK01.LAX"
struct Entry { std::string path; uint32_t lba; uint32_t size; bool dir; };    // path as given to build (no ";1"); lba: absolute, first data block
namespace detail {
inline void le16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void be16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
inline void be32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }
inline void both16(uint8_t* p, uint32_t v) { le16(p, v); be16(p + 2, v); }
inline void both32(uint8_t* p, uint32_t v) { le32(p, v); be32(p + 4, v); }
inline uint32_t rle16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
inline uint32_t rle32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
inline bool dchars(const std::string& s, size_t lo, size_t hi) {   // d-characters: A-Z 0-9 _
    if (s.size() < lo || s.size() > hi) return false;
    for (char c : s) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false; return true; }
inline std::string ucs2(const std::string& a) { std::string u; for (char c : a) { u += '\0'; u += c; } return u; }   // ASCII -> UCS-2 BE
// descriptor text field: SPACE-padded characters, or (Joliet) UCS-2 BE padded with UCS-2 SPACE; an odd last byte stays 0
inline void text(uint8_t* p, size_t n, const std::string& s, bool joliet) {
    std::string v = joliet ? ucs2(s) : s; for (size_t i = 0; i < n; i++) p[i] = i < v.size() ? (uint8_t)v[i] : !joliet ? 0x20 : i % 2 ? 0x20 : 0; }
struct Node { std::string name, ext; bool dir = false; int parent = 0; const std::vector<uint8_t>* data = nullptr; std::vector<int> kids; uint32_t lba[2] = {0, 0}, len[2] = {0, 0}; };
// identifier in tree t (0 primary, 1 Joliet): files NAME.EXT;1 (Joliet drops the '.' when there is no extension), directories NAME
inline std::string ident(const Node& n, int t) {
    std::string s = n.dir ? n.name : n.name + (t == 0 || !n.ext.empty() ? "." : "") + n.ext + ";1"; return t ? ucs2(s) : s; }
inline uint32_t recLen(size_t idLen) { return (uint32_t)(33 + idLen + (idLen % 2 == 0)); }   // padding byte keeps records even
// a directory record never crosses a sector boundary (ECMA-119 6.8.1.1): one that would starts the next sector
inline uint64_t place(uint64_t& pos, uint32_t rl) { if (pos % 2048 + rl > 2048) pos = (pos / 2048 + 1) * 2048; uint64_t at = pos; pos += rl; return at; }
inline void record(uint8_t* p, const std::string& id, uint32_t lba, uint32_t len, bool dir, const uint8_t* date7) {
    p[0] = (uint8_t)recLen(id.size()); both32(p + 2, lba); both32(p + 10, len); memcpy(p + 18, date7, 7);
    p[25] = dir ? 2 : 0; both16(p + 28, 1); p[32] = (uint8_t)id.size(); memcpy(p + 33, id.data(), id.size()); }
inline bool walk(const std::vector<uint8_t>& im, uint32_t startLba, bool joliet, uint32_t loc, uint32_t len, const std::string& pre, int depth,
                 std::set<uint32_t>& seen, std::vector<Entry>& out, std::string& err) {
    if (depth > 64 || !seen.insert(loc).second) { err = "directory loop or hierarchy too deep"; return false; }
    uint64_t ns = im.size() / 2048;
    for (uint64_t off = 0; off < len;) {
        uint64_t s = (uint64_t)loc + off / 2048; if (s < startLba || s - startLba >= ns) { err = "directory extent outside the image"; return false; }
        const uint8_t* r = &im[(size_t)(s - startLba) * 2048 + off % 2048]; uint32_t rl = r[0];
        if (!rl) { off = (off / 2048 + 1) * 2048; continue; }   // rest of the sector unused
        if (rl < 34 || off % 2048 + rl > 2048) { err = "bad directory record"; return false; }
        const uint32_t n = r[32]; if (33 + n > rl) { err = "bad directory record"; return false; }   // the identifier length is read only inside the record
        off += rl; const uint8_t* id = r + 33;
        if (n == 1 && id[0] <= 1) continue;   // "." and ".."
        bool dir = (r[25] & 2) != 0; if (r[25] & 0x80) { err = "multi-extent files are not supported"; return false; }
        std::string nm;
        if (joliet) { if (n % 2) { err = "odd-length Joliet identifier"; return false; }
            for (uint32_t i = 0; i < n; i += 2) { uint32_t c = (uint32_t)id[i] << 8 | id[i + 1];   // UCS-2 BE -> UTF-8
                if (c < 0x80) nm += (char)c; else if (c < 0x800) { nm += (char)(0xC0 | c >> 6); nm += (char)(0x80 | (c & 63)); }
                else { nm += (char)(0xE0 | c >> 12); nm += (char)(0x80 | (c >> 6 & 63)); nm += (char)(0x80 | (c & 63)); } } }
        else nm.assign((const char*)id, n);
        if (!dir) { size_t sc = nm.find(';'); if (sc != std::string::npos) nm.resize(sc); if (!nm.empty() && nm.back() == '.') nm.pop_back(); }
        if (nm.empty() || nm == "." || nm == ".." || nm.find_first_of(std::string("/\\\0", 3)) != std::string::npos) { err = "bad file identifier"; return false; }
        Entry e{pre + nm, (uint32_t)(rle32(r + 2) + r[1]), rle32(r + 10), dir}; out.push_back(e);   // data follows any extended attribute record
        if (dir && !walk(im, startLba, joliet, e.lba, e.size, e.path + "/", depth + 1, seen, out, err)) return false; }
    return true; }
}
// Builds the volume. Names are ISO 9660 Level 1: files NAME[.EXT] with NAME 1-8 and EXT 1-3 of A-Z 0-9 _ (recorded with ";1"),
// directories 1-8 of the same, at most 8 levels including the root; the Joliet tree carries the same names in UCS-2 BE. Directories
// are implied by the file paths. volumeId: 1-32 of A-Z 0-9 _ (the Joliet copy keeps the first 16). utcStamp "YYYYMMDDhhmmss" (UTC,
// years 1900-2155) is the creation and modification date and the date of every directory record. After the last file come
// tailZeroSectors zero sectors, more if the volume would be shorter than minTotalSectors. Output depends only on the set of files,
// not on their order. Returns the volume (relative sector 0 first), or empty with *err set.
inline std::vector<uint8_t> build(const std::vector<File>& files, uint32_t startLba, const std::string& volumeId, const std::string& utcStamp,
                                  uint32_t tailZeroSectors = 150, uint32_t minTotalSectors = 300, std::string* err = nullptr) {
    using namespace detail; auto fail = [&](const std::string& m) { if (err) *err = m; return std::vector<uint8_t>(); };
    if (!dchars(volumeId, 1, 32)) return fail("volume identifier must be 1-32 characters of A-Z 0-9 _");
    bool digits = utcStamp.size() == 14; for (char c : utcStamp) digits = digits && c >= '0' && c <= '9';
    if (!digits) return fail("utcStamp must be YYYYMMDDhhmmss");
    auto dg = [&](int a, int n) { int v = 0; for (int i = a; i < a + n; i++) v = v * 10 + (utcStamp[i] - '0'); return v; };
    int Y = dg(0, 4), Mo = dg(4, 2), D = dg(6, 2), h = dg(8, 2), mi = dg(10, 2), s = dg(12, 2); static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (Y < 1900 || Y > 2155 || Mo < 1 || Mo > 12 || D < 1 || D > dm[Mo - 1] + (Mo == 2 && Y % 4 == 0 && (Y % 100 || Y % 400 == 0)) || h > 23 || mi > 59 || s > 59)
        return fail("utcStamp out of range: " + utcStamp);
    const uint8_t d7[7] = {(uint8_t)(Y - 1900), (uint8_t)Mo, (uint8_t)D, (uint8_t)h, (uint8_t)mi, (uint8_t)s, 0};   // offset 0: UTC
    const std::string d17 = utcStamp + "00";   // 17-byte date: 16 digits (hundredths 00), offset byte stays 0
    std::vector<Node> nd(1); nd[0].dir = true; std::map<std::pair<int, std::string>, int> byName;   // (parent, name as shown) -> node
    for (const File& f : files) {
        if ((uint64_t)f.data.size() > 0xFFFFFFFFull) return fail("file larger than 4 GB - 1: " + f.path);
        int cur = 0, level = 1; size_t a = 0;
        for (size_t b; (b = f.path.find('/', a)) != std::string::npos; a = b + 1) {
            std::string c = f.path.substr(a, b - a);
            if (!dchars(c, 1, 8)) return fail("invalid ISO 9660 Level 1 directory name in " + f.path);
            if (++level > 8) return fail("more than 8 directory levels: " + f.path);
            auto it = byName.find({cur, c});
            if (it == byName.end()) { Node n; n.name = c; n.dir = true; n.parent = cur; nd.push_back(n); int k = (int)nd.size() - 1; byName[{cur, c}] = k; nd[cur].kids.push_back(k); cur = k; }
            else if (!nd[it->second].dir) return fail("name used for a file and a directory: " + f.path);
            else cur = it->second; }
        std::string c = f.path.substr(a); size_t dot = c.find('.'); Node n; n.name = c.substr(0, dot); if (dot != std::string::npos) n.ext = c.substr(dot + 1);
        if (!dchars(n.name, 1, 8) || (dot != std::string::npos && !dchars(n.ext, 1, 3))) return fail("invalid ISO 9660 Level 1 file name in " + f.path);
        if (byName.count({cur, c})) return fail("duplicate path or name clash: " + f.path);
        n.parent = cur; n.data = &f.data; nd.push_back(n); int k = (int)nd.size() - 1; byName[{cur, c}] = k; nd[cur].kids.push_back(k); }
    // ECMA-119 9.3 order: name, then extension, each padded with SPACE; every d-character sorts above SPACE, so comparing the parts is that order
    for (Node& n : nd) std::sort(n.kids.begin(), n.kids.end(), [&](int x, int y) { return std::tie(nd[x].name, nd[x].ext) < std::tie(nd[y].name, nd[y].ext); });
    // path-table order (6.9.1): by level, then parent number, then identifier = breadth-first over the sorted tree
    std::vector<int> dirs{0}; for (size_t i = 0; i < dirs.size(); i++) for (int k : nd[dirs[i]].kids) if (nd[k].dir) dirs.push_back(k);
    if (dirs.size() > 65535) return fail("more than 65535 directories");   // parent numbers are 16-bit
    std::vector<uint32_t> no(nd.size()); for (size_t i = 0; i < dirs.size(); i++) no[dirs[i]] = (uint32_t)i + 1;
    uint64_t pt[2] = {0, 0};
    for (int t = 0; t < 2; t++) for (int d : dirs) {
        uint64_t pos = 68; for (int k : nd[d].kids) place(pos, recLen(ident(nd[k], t).size()));   // after "." and ".."
        if (pos > 0xFFFFF800u) return fail("directory too large"); nd[d].len[t] = (uint32_t)((pos + 2047) / 2048 * 2048);
        size_t n = d ? ident(nd[d], t).size() : 1; pt[t] += 8 + n + (n & 1); }
    uint64_t rel = 19, ptL[2], ptM[2];
    for (int t = 0; t < 2; t++) { uint64_t n = (pt[t] + 2047) / 2048; ptL[t] = startLba + rel; rel += n; ptM[t] = startLba + rel; rel += n; }
    for (int t = 0; t < 2; t++) for (int d : dirs) { nd[d].lba[t] = (uint32_t)(startLba + rel); rel += nd[d].len[t] / 2048; }
    for (int d : dirs) for (int k : nd[d].kids) if (!nd[k].dir) { Node& n = nd[k];   // an empty file takes no block; its extent points at the next free one
        n.lba[0] = n.lba[1] = (uint32_t)(startLba + rel); n.len[0] = n.len[1] = (uint32_t)n.data->size(); rel += (n.data->size() + 2047) / 2048; }
    uint64_t total = std::max<uint64_t>(rel + tailZeroSectors, minTotalSectors);
    if (startLba + total > 0xFFFFFFFFull) return fail("volume does not fit 32-bit LBAs");
    if (total > (size_t)-1 / 2048) return fail("volume too large for memory");
    std::vector<uint8_t> im((size_t)total * 2048, 0); auto S = [&](uint64_t abs) { return &im[(size_t)(abs - startLba) * 2048]; };
    for (int t = 0; t < 2; t++) { uint8_t* p = &im[(size_t)(16 + t) * 2048]; bool j = t == 1;
        p[0] = j ? 2 : 1; memcpy(p + 1, "CD001", 5); p[6] = 1;
        text(p + 8, 32, "", j); text(p + 40, 32, j ? volumeId.substr(0, 16) : volumeId, j); both32(p + 80, (uint32_t)(startLba + total));
        if (j) { p[88] = 0x25; p[89] = 0x2F; p[90] = 0x45; }   // "%/E": UCS-2 Level 3
        both16(p + 120, 1); both16(p + 124, 1); both16(p + 128, 2048); both32(p + 132, (uint32_t)pt[t]); le32(p + 140, (uint32_t)ptL[t]); be32(p + 148, (uint32_t)ptM[t]);
        record(p + 156, std::string(1, '\0'), nd[0].lba[t], nd[0].len[t], true, d7);
        text(p + 190, 128, "", j); text(p + 318, 128, "", j); text(p + 446, 128, "", j); text(p + 574, 128, "LAE", j);   // set, publisher, preparer, application
        text(p + 702, 37, "", j); text(p + 739, 37, "", j); text(p + 776, 37, "", j);   // copyright, abstract, bibliographic files
        memcpy(p + 813, d17.data(), 16); memcpy(p + 830, d17.data(), 16); memset(p + 847, '0', 16); memset(p + 864, '0', 16);   // expiration, effective: not specified
        p[881] = 1; }
    { uint8_t* p = &im[18 * 2048]; p[0] = 255; memcpy(p + 1, "CD001", 5); p[6] = 1; }
    for (int t = 0; t < 2; t++) { uint8_t *L = S(ptL[t]), *M = S(ptM[t]); size_t o = 0;
        for (int d : dirs) { std::string id = d ? ident(nd[d], t) : std::string(1, '\0'); size_t n = id.size(); uint32_t par = no[nd[d].parent];
            L[o] = M[o] = (uint8_t)n; le32(L + o + 2, nd[d].lba[t]); be32(M + o + 2, nd[d].lba[t]); le16(L + o + 6, par); be16(M + o + 6, par);
            memcpy(L + o + 8, id.data(), n); memcpy(M + o + 8, id.data(), n); o += 8 + n + (n & 1); } }
    for (int t = 0; t < 2; t++) for (int d : dirs) { uint8_t* p = S(nd[d].lba[t]); const Node &n = nd[d], &pa = nd[n.parent]; uint64_t pos = 0;
        record(p + place(pos, 34), std::string(1, '\0'), n.lba[t], n.len[t], true, d7); record(p + place(pos, 34), std::string(1, '\1'), pa.lba[t], pa.len[t], true, d7);
        for (int k : n.kids) { std::string id = ident(nd[k], t); record(p + place(pos, recLen(id.size())), id, nd[k].lba[t], nd[k].len[t], nd[k].dir, d7); } }
    for (const Node& n : nd) if (!n.dir && !n.data->empty()) memcpy(S(n.lba[0]), n.data->data(), n.data->size());
    if (err) err->clear(); return im; }
// Lists every file and directory (pre-order, in recorded order) of the primary tree, or of the Joliet tree when joliet = true.
// image: the volume, its sector 0 at absolute LBA startLba.
inline bool list(const std::vector<uint8_t>& image, uint32_t startLba, bool joliet, std::vector<Entry>& out, std::string& err) {
    using namespace detail; out.clear(); const uint8_t* vd = nullptr;
    for (size_t s = 16; s < image.size() / 2048 && !vd; s++) { const uint8_t* p = &image[s * 2048];
        if (memcmp(p + 1, "CD001", 5) || p[0] == 255) break;
        if (joliet ? p[0] == 2 && p[88] == 0x25 && p[89] == 0x2F && (p[90] == 0x40 || p[90] == 0x43 || p[90] == 0x45) : p[0] == 1) vd = p; }
    if (!vd) { err = joliet ? "no Joliet volume descriptor" : "no primary volume descriptor"; return false; }
    if (rle16(vd + 128) != 2048) { err = "logical block size is not 2048"; return false; }
    std::set<uint32_t> seen; return walk(image, startLba, joliet, rle32(vd + 158) + vd[157], rle32(vd + 166), "", 1, seen, out, err); }
inline bool readFile(const std::vector<uint8_t>& image, uint32_t startLba, const Entry& e, std::vector<uint8_t>& out) {
    out.clear(); if (e.dir) return false; if (!e.size) return true;
    if (e.lba < startLba || (uint64_t)(e.lba - startLba) * 2048 + e.size > image.size()) return false;
    size_t o = (size_t)(e.lba - startLba) * 2048; out.assign(image.begin() + o, image.begin() + o + e.size); return true; }
}
