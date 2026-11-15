/*
 * fun7z —— 7z 归档的只读内存挂载器（纯 C，配 liblzma）
 *
 * 设计目标：像游戏引擎读资源包那样，把 .fun 当成一个只读文件系统挂起来。
 *   - 不落地：解压结果只存在于内存，磁盘上不产生任何文件
 *   - 随机读：可以只读其中某一个条目，不必展开整包
 *   - 可嵌入：fun_open / fun_read / fun_close 就是一套资源加载 API
 *
 * 用法：
 *   funread list <archive.fun>
 *   funread cat  <archive.fun> <entry>
 *   funread head <archive.fun> <entry> <n>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <lzma.h>

#define FUN_MAX_PACK     64
#define FUN_MAX_FOLDERS  64
#define FUN_MAX_CODERS   8
#define FUN_MAX_FILES    512
#define FUN_MAX_SUBS     512
#define FUN_MAX_NAME     300

/* ---------------------------------------------------------------- 7z 结构 */

typedef struct {
    uint32_t id;          /* 0x21 LZMA2 / 0x030101 LZMA1 / 0x03030103 BCJ-x86 / 0 Copy */
    uint8_t  props[32];
    int      propsLen;
    int      nIn, nOut;
    int      firstIn, firstOut;
} Coder;

typedef struct {
    int     nCoders;
    Coder   coder[FUN_MAX_CODERS];
    int     nBindPairs;
    int     bindIn[FUN_MAX_CODERS], bindOut[FUN_MAX_CODERS];
    int     nPacked;
    int     packedIdx[FUN_MAX_CODERS];
    uint64_t unpackSize;          /* 整个 folder 解出来的长度 */
    int     nSubs;
    uint64_t subSize[FUN_MAX_SUBS];
} Folder;

typedef struct {
    char     name[FUN_MAX_NAME];
    uint64_t size;
    int      isDir;
} Entry;

typedef struct {
    uint8_t *buf;
    uint64_t len;

    uint64_t packPos;
    int      nPacks;
    uint64_t packSize[FUN_MAX_PACK];

    int      nFolders;
    Folder   folder[FUN_MAX_FOLDERS];

    int      nFiles;
    Entry    file[FUN_MAX_FILES];

    /* 每个文件落在哪个 folder、在 folder 内的偏移 */
    int      fileFolder[FUN_MAX_FILES];
    uint64_t fileOffset[FUN_MAX_FILES];

    int      nSubs;                /* 所有 folder 的 substream 总数 */
    uint64_t subSize[FUN_MAX_SUBS];
} Fun;

/* ---------------------------------------------------------------- 位流读取 */

typedef struct {
    const uint8_t *p;
    uint64_t       left;
} BR;

static uint8_t br_byte(BR *b){
    if (b->left == 0){ fprintf(stderr, "fun7z: header truncated\n"); exit(3); }
    b->left--; return *b->p++;
}

static uint64_t br_num(BR *b){
    uint64_t first = br_byte(b);
    uint64_t mask = 0x80, value = 0;
    for (int i = 0; i < 8; i++){
        if ((first & mask) == 0){
            uint64_t high = first & (mask - 1);
            value |= high << (i * 8);
            return value;
        }
        value |= ((uint64_t)br_byte(b)) << (8 * i);
        mask >>= 1;
    }
    return value;
}

static uint32_t br_u32(BR *b){
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= ((uint32_t)br_byte(b)) << (8 * i);
    return v;
}

/* allAreDefined + 位向量 */
static int *br_bits(BR *b, int n){
    int *out = (int*)calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    uint8_t all = br_byte(b);
    if (all){
        for (int i = 0; i < n; i++) out[i] = 1;
    } else {
        uint8_t mask = 0, cur = 0;
        for (int i = 0; i < n; i++){
            if (mask == 0){ cur = br_byte(b); mask = 0x80; }
            out[i] = (cur & mask) ? 1 : 0;
            mask >>= 1;
        }
    }
    return out;
}

/* 纯位向量：没有 allAreDefined 头字节（kEmptyStream / kEmptyFile 用这种） */
static int *br_bits_raw(BR *b, int n){
    int *out = (int*)calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    uint8_t mask = 0, cur = 0;
    for (int i = 0; i < n; i++){
        if (mask == 0){ cur = br_byte(b); mask = 0x80; }
        out[i] = (cur & mask) ? 1 : 0;
        mask >>= 1;
    }
    return out;
}

/* ---------------------------------------------------------------- folder */

static void parse_folder(BR *b, Folder *f){
    memset(f, 0, sizeof(*f));
    uint64_t n = br_num(b);
    if (n > FUN_MAX_CODERS){ fprintf(stderr, "fun7z: too many coders %llu\n", (unsigned long long)n); exit(3); }
    f->nCoders = (int)n;

    int totalIn = 0, totalOut = 0;
    for (int i = 0; i < f->nCoders; i++){
        Coder *c = &f->coder[i];
        uint8_t flags = br_byte(b);
        int idSize = flags & 0x0F;
        int isComplex = (flags & 0x10) ? 1 : 0;
        int hasAttrs = (flags & 0x20) ? 1 : 0;

        c->id = 0;
        for (int k = 0; k < idSize; k++){
            uint8_t bb = br_byte(b);
            c->id = (c->id << 8) | bb;
        }
        c->nIn = 1; c->nOut = 1;
        if (isComplex){
            c->nIn  = (int)br_num(b);
            c->nOut = (int)br_num(b);
        }
        c->firstIn = totalIn; c->firstOut = totalOut;
        totalIn += c->nIn; totalOut += c->nOut;

        c->propsLen = 0;
        if (hasAttrs){
            uint64_t sz = br_num(b);
            if (sz > 32) sz = 32;
            c->propsLen = (int)sz;
            for (int k = 0; k < (int)sz; k++) c->props[k] = br_byte(b);
        }
    }

    int nBind = totalOut - 1;
    if (nBind > FUN_MAX_CODERS){ fprintf(stderr, "fun7z: bad bind count\n"); exit(3); }
    f->nBindPairs = nBind;
    for (int i = 0; i < nBind; i++){
        f->bindIn[i]  = (int)br_num(b);
        f->bindOut[i] = (int)br_num(b);
    }
    int nPacked = totalIn - nBind;
    f->nPacked = nPacked;
    if (nPacked > 1){
        for (int i = 0; i < nPacked; i++) f->packedIdx[i] = (int)br_num(b);
    } else if (nPacked == 1){
        /* 只有一个包流时 7z 不写索引，它就是那个没被绑定的输入 */
        f->packedIdx[0] = 0;
        for (int in = 0; in < totalIn; in++){
            int bound = 0;
            for (int i = 0; i < nBind; i++) if (f->bindIn[i] == in) bound = 1;
            if (!bound){ f->packedIdx[0] = in; break; }
        }
    }

    /* 最终输出流：没有被 bind 的 out */
    int finalOut = 0;
    for (int o = 0; o < totalOut; o++){
        int used = 0;
        for (int i = 0; i < nBind; i++) if (f->bindOut[i] == o) used = 1;
        if (!used) finalOut = o;
    }
    f->unpackSize = 0;   /* 稍后由 kCodersUnpackSize 填入 */
    (void)finalOut;
}

/* folder 里最终输出流的下标 */
static int folder_final_out(const Folder *f){
    int totalOut = 0;
    for (int i = 0; i < f->nCoders; i++) totalOut += f->coder[i].nOut;
    for (int o = 0; o < totalOut; o++){
        int used = 0;
        for (int i = 0; i < f->nBindPairs; i++) if (f->bindOut[i] == o) used = 1;
        if (!used) return o;
    }
    return 0;
}

/* ---------------------------------------------------------------- 头部解析 */

typedef struct {
    uint64_t packPos;
    int      nPacks;
    uint64_t packSize[FUN_MAX_PACK];
    int      nFolders;
    Folder   folder[FUN_MAX_FOLDERS];
    int      nSubs;
    uint64_t subSize[FUN_MAX_SUBS];
    int      subCount[FUN_MAX_FOLDERS];
} Streams;

static void parse_streams(BR *b, Streams *s){
    memset(s, 0, sizeof(*s));

    while (1){
        uint8_t id = br_byte(b);
        if (id == 0x00) break;

        if (id == 0x06){                       /* kPackInfo */
            s->packPos = br_num(b);
            s->nPacks = (int)br_num(b);
            while (1){
                uint8_t pid = br_byte(b);
                if (pid == 0x00) break;
                if (pid == 0x09){
                    for (int i = 0; i < s->nPacks; i++) s->packSize[i] = br_num(b);
                } else if (pid == 0x0A){
                    int *def = br_bits(b, s->nPacks);
                    for (int i = 0; i < s->nPacks; i++) if (def[i]) br_u32(b);
                    free(def);
                } else {
                    fprintf(stderr, "fun7z: unknown pack prop 0x%02x\n", pid); exit(3);
                }
            }
        } else if (id == 0x07){                /* kUnpackInfo */
            uint8_t fid = br_byte(b);
            if (fid != 0x0B){ fprintf(stderr, "fun7z: expect kFolder\n"); exit(3); }
            s->nFolders = (int)br_num(b);
            uint8_t external = br_byte(b);
            if (external){ fprintf(stderr, "fun7z: external folders unsupported\n"); exit(3); }
            for (int i = 0; i < s->nFolders; i++) parse_folder(b, &s->folder[i]);

            uint8_t sid = br_byte(b);
            if (sid != 0x0C){ fprintf(stderr, "fun7z: expect kCodersUnpackSize\n"); exit(3); }
            for (int i = 0; i < s->nFolders; i++){
                Folder *f = &s->folder[i];
                int totalOut = 0;
                for (int c = 0; c < f->nCoders; c++) totalOut += f->coder[c].nOut;
                for (int o = 0; o < totalOut; o++){
                    uint64_t sz = br_num(b);
                    if (o == folder_final_out(f)) f->unpackSize = sz;
                }
            }
            /* 可选 CRC */
            while (1){
                uint8_t pid = br_byte(b);
                if (pid == 0x00) break;
                if (pid == 0x0A){
                    int *def = br_bits(b, s->nFolders);
                    for (int i = 0; i < s->nFolders; i++) if (def[i]) br_u32(b);
                    free(def);
                } else {
                    fprintf(stderr, "fun7z: unknown unpack prop 0x%02x\n", pid); exit(3);
                }
            }
        } else if (id == 0x08){                /* kSubStreamsInfo */
            for (int i = 0; i < s->nFolders; i++) s->subCount[i] = 1;
            while (1){
                uint8_t pid = br_byte(b);
                if (pid == 0x00) break;
                if (pid == 0x0D){                 /* kNumUnpackStream */
                    for (int i = 0; i < s->nFolders; i++) s->subCount[i] = (int)br_num(b);
                } else if (pid == 0x09){          /* kSize */
                    for (int i = 0; i < s->nFolders; i++){
                        Folder *f = &s->folder[i];
                        int n = s->subCount[i];
                        uint64_t sum = 0;
                        for (int k = 0; k < n - 1; k++){
                            uint64_t sz = br_num(b);
                            s->subSize[s->nSubs++] = sz; sum += sz;
                        }
                        if (n > 0) s->subSize[s->nSubs++] = f->unpackSize - sum;
                    }
                } else if (pid == 0x0A){          /* kCRC */
                    int total = 0;
                    for (int i = 0; i < s->nFolders; i++) total += s->subCount[i];
                    int *def = br_bits(b, total);
                    for (int i = 0; i < total; i++) if (def[i]) br_u32(b);
                    free(def);
                } else {
                    fprintf(stderr, "fun7z: unknown substream prop 0x%02x\n", pid); exit(3);
                }
            }
            /* 没有 kSize 的 folder，只有一个子流，长度即 folder 长度 */
            int have = 0;
            for (int i = 0; i < s->nFolders; i++) have += s->subCount[i];
            if (have > s->nSubs && s->nSubs == 0){
                for (int i = 0; i < s->nFolders; i++) s->subSize[s->nSubs++] = s->folder[i].unpackSize;
            }
        } else {
            fprintf(stderr, "fun7z: unknown streams id 0x%02x\n", id); exit(3);
        }
    }
}

/* 从 UTF-16LE 名字序列里取第 idx 个 */
static void pick_name(const uint8_t *data, uint64_t len, int idx, char *out, int outLen){
    uint64_t i = 0; int n = 0; int o = 0;
    out[0] = 0;
    while (i + 1 < len){
        uint16_t c = (uint16_t)(data[i] | (data[i+1] << 8));
        i += 2;
        if (c == 0){
            if (n == idx) return;
            n++; o = 0;
            continue;
        }
        if (n == idx && o < outLen - 1){
            out[o++] = (char)(c < 128 ? c : '?');
            out[o] = 0;
        }
    }
}

/* ---------------------------------------------------------------- 解码 */

static void apply_coder_filters(const Folder *f, lzma_filter *filters, lzma_options_lzma *opt,
                                lzma_options_bcj *bcj){
    int nf = 0;
    memset(opt, 0, sizeof(*opt));

    /* 单 coder：最常见。双 coder 时按 BCJ + LZMAx 处理 */
    for (int i = f->nCoders - 1; i >= 0; i--){
        const Coder *c = &f->coder[i];
        if (c->id == 0x03030103){                 /* BCJ x86 */
            filters[nf].id = LZMA_FILTER_X86;
            filters[nf].options = bcj;
            nf++;
        } else if (c->id == 0x21){                /* LZMA2 */
            if (c->propsLen >= 1){
                uint8_t p = c->props[0];
                opt->dict_size = (uint32_t)((2 | (p & 1)) << (p / 2 + 11));
            } else opt->dict_size = 1u << 20;
            filters[nf].id = LZMA_FILTER_LZMA2;
            filters[nf].options = opt;
            nf++;
        } else if (c->id == 0x030101){            /* LZMA1 */
            if (c->propsLen >= 5){
                uint8_t p0 = c->props[0];
                opt->lc = p0 % 9;
                opt->lp = (p0 / 9) % 5;
                opt->pb = p0 / 45;
                opt->dict_size = (uint32_t)c->props[1] |
                                 ((uint32_t)c->props[2] << 8) |
                                 ((uint32_t)c->props[3] << 16) |
                                 ((uint32_t)c->props[4] << 24);
                if (opt->dict_size < (1u << 12)) opt->dict_size = 1u << 12;
            }
            filters[nf].id = LZMA_FILTER_LZMA1;
            filters[nf].options = opt;
            nf++;
        } else if (c->id == 0x00){                /* Copy */
            filters[nf].id = LZMA_VLI_UNKNOWN;
            nf = 0;
        } else {
            fprintf(stderr, "fun7z: unsupported coder 0x%06x\n", c->id);
            exit(4);
        }
    }
    filters[nf].id = LZMA_VLI_UNKNOWN;
    filters[nf].options = NULL;
}

static uint64_t data_base(const Fun *a){ return 32; }

static uint8_t *decode_folder(const Fun *a, int fi, uint64_t *outLen){
    const Folder *f = &a->folder[fi];

    /* 只支持每个 folder 一个 pack stream 的常见情况 */
    if (f->nPacked != 1){
        fprintf(stderr, "fun7z: folder with %d pack streams unsupported\n", f->nPacked);
        return NULL;
    }

    uint64_t pos = data_base(a) + a->packPos;
    for (int i = 0; i < f->packedIdx[0]; i++) pos += a->packSize[i];
    uint64_t packLen = a->packSize[f->packedIdx[0]];
    if (pos + packLen > a->len){
        fprintf(stderr, "fun7z: pack stream out of range (pos=%llu len=%llu pack=%llu total=%llu)\n",
                (unsigned long long)pos, (unsigned long long)packLen,
                (unsigned long long)a->packSize[f->packedIdx[0]], (unsigned long long)a->len);
        return NULL;
    }

    uint8_t *out = (uint8_t*)malloc((size_t)(f->unpackSize ? f->unpackSize : 1));
    if (!out) return NULL;

    lzma_filter filters[3];
    lzma_options_lzma opt;
    lzma_options_bcj bcj;
    memset(&bcj, 0, sizeof(bcj));
    apply_coder_filters(f, filters, &opt, &bcj);

    if (filters[0].id == LZMA_VLI_UNKNOWN){
        memcpy(out, a->buf + pos, (size_t)(packLen < f->unpackSize ? packLen : f->unpackSize));
        *outLen = packLen < f->unpackSize ? packLen : f->unpackSize;
        return out;
    }

    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret r = lzma_raw_decoder(&strm, filters);
    if (r != LZMA_OK){
        fprintf(stderr, "fun7z: raw decoder init failed (%d)\n", (int)r);
        free(out); return NULL;
    }
    strm.next_in = a->buf + pos;
    strm.avail_in = (size_t)packLen;
    strm.next_out = out;
    strm.avail_out = (size_t)f->unpackSize;

    int guard = 0;
    while (strm.avail_out > 0 && guard++ < 200000){
        r = lzma_code(&strm, LZMA_RUN);
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK){
            fprintf(stderr, "fun7z: decode error %d\n", (int)r);
            lzma_end(&strm); free(out); return NULL;
        }
        if (strm.avail_in == 0 && strm.avail_out > 0) break;
    }
    lzma_end(&strm);
    *outLen = f->unpackSize - strm.avail_out;
    return out;
}

/* ---------------------------------------------------------------- 公共 API */

typedef struct {
    uint8_t *buf;
    uint64_t len;
    Fun      f;
} FunArc;

static void read_all(const char *path, uint8_t **out, uint64_t *len){
    FILE *fp = fopen(path, "rb");
    if (!fp){ fprintf(stderr, "fun7z: cannot open %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = (uint8_t*)malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz){ fclose(fp); exit(2); }
    fclose(fp);
    *out = b; *len = (uint64_t)sz;
}

int fun_open(FunArc *arc, const char *path){
    memset(arc, 0, sizeof(*arc));
    read_all(path, &arc->buf, &arc->len);
    arc->f.buf = arc->buf;
    arc->f.len = arc->len;

    if (arc->len < 32 || memcmp(arc->buf, "7z\xBC\xAF\x27\x1C", 6) != 0){
        fprintf(stderr, "fun7z: not a 7z container\n");
        return 0;
    }

    uint64_t nhOff  = 0, nhSize = 0;
    for (int i = 0; i < 8; i++) nhOff  |= ((uint64_t)arc->buf[12 + i]) << (8 * i);
    for (int i = 0; i < 8; i++) nhSize |= ((uint64_t)arc->buf[20 + i]) << (8 * i);

    if (32 + nhOff + nhSize > arc->len){
        fprintf(stderr, "fun7z: header out of range\n");
        return 0;
    }

    BR b = { arc->buf + 32 + nhOff, nhSize };
    uint8_t *hdrBuf = NULL;
    uint8_t id = br_byte(&b);

    /* kEncodedHeader：索引本身被压成了包流，先解出来再当头部读 */
    if (id == 0x17){
        Streams *hs = (Streams*)calloc(1, sizeof(Streams));
        parse_streams(&b, hs);

        Fun tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.buf = arc->buf; tmp.len = arc->len;
        tmp.packPos = hs->packPos; tmp.nPacks = hs->nPacks;
        for (int i = 0; i < hs->nPacks; i++) tmp.packSize[i] = hs->packSize[i];
        tmp.nFolders = hs->nFolders;
        for (int i = 0; i < hs->nFolders; i++) tmp.folder[i] = hs->folder[i];

        uint64_t hl = 0;
        hdrBuf = decode_folder(&tmp, 0, &hl);
        free(hs);
        if (!hdrBuf){ fprintf(stderr, "fun7z: cannot decode header\n"); return 0; }

        b.p = hdrBuf; b.left = hl;
        id = br_byte(&b);
    }

    if (id != 0x01){ fprintf(stderr, "fun7z: no kHeader (0x%02x)\n", id); if (hdrBuf) free(hdrBuf); return 0; }

    Streams *st = (Streams*)calloc(1, sizeof(Streams));
    int haveStreams = 0;
    int nFiles = 0;

    while (1){
        uint8_t hid = br_byte(&b);
        if (hid == 0x00) break;
        if (hid == 0x02){                     /* kArchiveProperties */
            while (1){
                uint8_t pid = br_byte(&b);
                if (pid == 0x00) break;
                uint64_t sz = br_num(&b);
                for (uint64_t k = 0; k < sz; k++) br_byte(&b);
            }
        } else if (hid == 0x03){              /* kAdditionalStreamsInfo */
            parse_streams(&b, st);
        } else if (hid == 0x04){              /* kMainStreamsInfo */
            parse_streams(&b, st);
            haveStreams = 1;
        } else if (hid == 0x05){              /* kFilesInfo */
            uint64_t num = br_num(&b);
            nFiles = (int)num;
            if (nFiles > FUN_MAX_FILES) nFiles = FUN_MAX_FILES;

            int *empty = NULL, *emptyFile = NULL;
            while (1){
                uint8_t pid = br_byte(&b);
                if (pid == 0x00) break;
                uint64_t sz = br_num(&b);
                const uint8_t *start = b.p;
                if (pid == 0x0E){                        /* kEmptyStream */
                    BR tb = { start, sz };
                    empty = br_bits_raw(&tb, (int)num);
                } else if (pid == 0x0F){                 /* kEmptyFile */
                    int nEmpty = 0;
                    for (int i = 0; i < (int)num; i++) if (empty && empty[i]) nEmpty++;
                    BR tb = { start, sz };
                    emptyFile = br_bits_raw(&tb, nEmpty);
                } else if (pid == 0x11){                 /* kName */
                    uint8_t external = 0;
                    const uint8_t *p = start;
                    if (sz > 0){ external = *p++; }
                    if (!external){
                        for (int i = 0; i < nFiles; i++){
                            pick_name(start + 1, sz - 1, i, arc->f.file[i].name, FUN_MAX_NAME);
                            if (arc->f.file[i].name[0] == 0)
                                snprintf(arc->f.file[i].name, FUN_MAX_NAME, "entry_%d", i);
                        }
                    }
                }
                b.p = start + sz; b.left -= sz;
            }

            for (int i = 0; i < nFiles; i++){
                int isDir = 0;
                if (empty && empty[i]){
                    isDir = 1;
                    if (emptyFile){
                        int k = 0;
                        for (int j = 0; j < i; j++) if (empty[j]) k++;
                        if (k < (int)num && emptyFile[k]) isDir = 0;
                    }
                }
                arc->f.file[i].isDir = isDir;
            }
            free(empty); free(emptyFile);
        } else {
            fprintf(stderr, "fun7z: unknown header id 0x%02x\n", hid);
            free(st);
            return 0;
        }
    }

    if (!haveStreams){ free(st); return 0; }
    arc->f.packPos = st->packPos;
    arc->f.nPacks  = st->nPacks;
    for (int i = 0; i < st->nPacks; i++) arc->f.packSize[i] = st->packSize[i];

    /* 头部里的 streams：先解出文件夹表 */
    arc->f.nFolders = st->nFolders;
    for (int i = 0; i < st->nFolders; i++) arc->f.folder[i] = st->folder[i];

    /* 把 substream 依次分给非目录文件 */
    int sub = 0;
    uint64_t folderOff = 0;
    int folderIdx = 0;
    int inFolder = 0;
    for (int i = 0; i < nFiles; i++){
        arc->f.fileFolder[i] = -1;
        if (arc->f.file[i].isDir) continue;
        while (folderIdx < arc->f.nFolders && inFolder >= st->subCount[folderIdx]){
            folderIdx++; inFolder = 0; folderOff = 0;
        }
        if (folderIdx >= arc->f.nFolders) break;
        uint64_t sz = 0;
        if (sub < st->nSubs) sz = st->subSize[sub];
        else sz = arc->f.folder[folderIdx].unpackSize;
        arc->f.file[i].size = sz;
        arc->f.fileFolder[i] = folderIdx;
        arc->f.fileOffset[i] = folderOff;
        folderOff += sz;
        inFolder++; sub++;
    }
    arc->f.nFiles = nFiles;

    free(st);
    if (hdrBuf) free(hdrBuf);
    return 1;
}

/* 读一个条目到内存（不落盘） */
uint8_t *fun_read(FunArc *arc, int idx, uint64_t *outLen){
    if (idx < 0 || idx >= arc->f.nFiles) return NULL;
    int fi = arc->f.fileFolder[idx];
    if (fi < 0) return NULL;

    uint64_t folderLen = 0;
    uint8_t *folder = decode_folder(&arc->f, fi, &folderLen);
    if (!folder) return NULL;

    uint64_t off = arc->f.fileOffset[idx];
    uint64_t len = arc->f.file[idx].size;
    if (off + len > folderLen) len = folderLen > off ? folderLen - off : 0;

    uint8_t *out = (uint8_t*)malloc((size_t)(len ? len : 1));
    if (len) memcpy(out, folder + off, (size_t)len);
    free(folder);
    *outLen = len;
    return out;
}

int fun_find(FunArc *arc, const char *name){
    for (int i = 0; i < arc->f.nFiles; i++)
        if (strcmp(arc->f.file[i].name, name) == 0) return i;
    return -1;
}

void fun_close(FunArc *arc){
    free(arc->buf);
    arc->buf = NULL;
}

/* ---------------------------------------------------------------- CRC32 */

static uint32_t crc32_buf(const uint8_t *p, uint64_t n){
    static uint32_t tab[256];
    static int init = 0;
    if (!init){
        for (uint32_t i = 0; i < 256; i++){
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint64_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------- CLI */

int main(int argc, char **argv){
    if (argc < 3){
        fprintf(stderr, "usage: funread list <a.fun>\n"
                        "       funread cat  <a.fun> <entry>\n"
                        "       funread head <a.fun> <entry> <bytes>\n");
        return 1;
    }

    FunArc arc;
    if (!fun_open(&arc, argv[2])) return 2;

    if (strcmp(argv[1], "check") == 0){
        int bad = 0;
        printf("%-40s %10s %10s %8s\n", "entry", "declared", "read", "crc32");
        for (int i = 0; i < arc.f.nFiles; i++){
            Entry *e = &arc.f.file[i];
            if (e->isDir) continue;
            uint64_t len = 0;
            uint8_t *data = fun_read(&arc, i, &len);
            if (!data){ printf("%-40s  DECODE FAILED\n", e->name); bad++; continue; }
            int ok = (len == e->size);
            printf("%-40s %10llu %10llu %08x%s\n", e->name,
                   (unsigned long long)e->size, (unsigned long long)len,
                   crc32_buf(data, len), ok ? "" : "  SIZE-MISMATCH");
            if (!ok) bad++;
            free(data);
        }
        fun_close(&arc);
        return bad ? 7 : 0;
    } else if (strcmp(argv[1], "info") == 0){
        printf("file_len=%llu packPos=%llu nPacks=%d\n",
               (unsigned long long)arc.len,
               (unsigned long long)arc.f.packPos, arc.f.nPacks);
        for (int i = 0; i < arc.f.nPacks; i++)
            printf("  pack[%d] size=%llu\n", i, (unsigned long long)arc.f.packSize[i]);
        for (int i = 0; i < arc.f.nFolders; i++){
            Folder *f = &arc.f.folder[i];
            printf("  folder[%d] coders=%d nPacked=%d unpack=%llu finalOut=%d\n",
                   i, f->nCoders, f->nPacked, (unsigned long long)f->unpackSize,
                   folder_final_out(f));
            for (int c = 0; c < f->nCoders; c++)
                printf("    coder id=0x%06x propsLen=%d\n", f->coder[c].id, f->coder[c].propsLen);
        }
    } else if (strcmp(argv[1], "list") == 0){
        printf("%d entries\n", arc.f.nFiles);
        for (int i = 0; i < arc.f.nFiles; i++){
            Entry *e = &arc.f.file[i];
            if (e->isDir) printf("  dir   %s\n", e->name[0] ? e->name : "(unnamed)");
            else          printf("  %6llu  %s\n", (unsigned long long)e->size, e->name);
        }
    } else if (strcmp(argv[1], "cat") == 0 && argc >= 4){
        int idx = fun_find(&arc, argv[3]);
        if (idx < 0){ fprintf(stderr, "fun7z: no entry %s\n", argv[3]); fun_close(&arc); return 5; }
        uint64_t len = 0;
        uint8_t *data = fun_read(&arc, idx, &len);
        if (!data){ fun_close(&arc); return 6; }
        fwrite(data, 1, (size_t)len, stdout);
        free(data);
    } else if (strcmp(argv[1], "head") == 0 && argc >= 5){
        int idx = fun_find(&arc, argv[3]);
        if (idx < 0){ fprintf(stderr, "fun7z: no entry %s\n", argv[3]); fun_close(&arc); return 5; }
        uint64_t len = 0;
        uint8_t *data = fun_read(&arc, idx, &len);
        if (!data){ fun_close(&arc); return 6; }
        uint64_t n = (uint64_t)atoi(argv[4]);
        if (n > len) n = len;
        fwrite(data, 1, (size_t)n, stdout);
        free(data);
    } else {
        fprintf(stderr, "fun7z: bad command\n");
        fun_close(&arc);
        return 1;
    }

    fun_close(&arc);
    return 0;
}
