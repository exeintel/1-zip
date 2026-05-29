#define LIB1ZIP_EXPORTS
#include "lib1zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>
#include <dirent.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define mkdir_p(path) _mkdir(path)
#else
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

#define ZIP_LOCAL_HEADER_SIG   0x04034b50
#define ZIP_CENTRAL_SIG        0x02014b50
#define ZIP_END_SIG            0x06054b50
#define ZIP_LOCAL_HEADER_SIZE  30
#define ZIP_CENTRAL_SIZE       46
#define ZIP_END_SIZE           22

#define HDR_1Z_MAGIC     "1Z"
#define HDR_AF_MAGIC     "AF"
#define HDR_1Z_VERSION   1
#define HDR_AF_VERSION   1

#pragma pack(push, 1)
typedef struct {
    unsigned int   sig;
    unsigned short version_needed;
    unsigned short flags;
    unsigned short method;
    unsigned short mod_time;
    unsigned short mod_date;
    unsigned int   crc32;
    unsigned int   comp_size;
    unsigned int   uncomp_size;
    unsigned short filename_len;
    unsigned short extra_len;
} ZipLocalHeader;

typedef struct {
    unsigned int   sig;
    unsigned short made_by;
    unsigned short version_needed;
    unsigned short flags;
    unsigned short method;
    unsigned short mod_time;
    unsigned short mod_date;
    unsigned int   crc32;
    unsigned int   comp_size;
    unsigned int   uncomp_size;
    unsigned short filename_len;
    unsigned short extra_len;
    unsigned short comment_len;
    unsigned short disk_start;
    unsigned short internal_attr;
    unsigned int   external_attr;
    unsigned int   offset;
} ZipCentralHeader;

typedef struct {
    unsigned int   sig;
    unsigned short disk_num;
    unsigned short disk_central;
    unsigned short entries_disk;
    unsigned short entries_total;
    unsigned int   central_size;
    unsigned int   central_offset;
    unsigned short comment_len;
} ZipEndCentral;
#pragma pack(pop)

typedef struct {
    char   name[1024];
    unsigned long crc32;
    unsigned int  comp_size;
    unsigned int  uncomp_size;
    int    method;
    unsigned short dos_time;
    unsigned short dos_date;
    long   header_offset;
} ZipPendingEntry;

struct L1Z_Archive {
    char            path[1024];
    int             format;
    char            mode[8];
    FILE*           fp;
    L1Z_Entry*      entries;
    int             entry_count;
    int             modified;
    unsigned long long comp_level;
    ZipPendingEntry* zip_pending;
    int             zip_pending_count;
    int             zip_pending_cap;
};

static void (*progress_cb)(const char* file, int percent) = NULL;

void l1z_set_progress_callback(void (*cb)(const char* file, int percent)) {
    progress_cb = cb;
}

static void report_progress(const char* file, int percent) {
    if (progress_cb) progress_cb(file ? file : "", percent);
}

static int is_leaf_filename(const char* path) {
    const char* p = strrchr(path, '/');
    const char* q = strrchr(path, '\\');
    const char* r = p > q ? p : q;
    return r ? (int)(r - path) + 1 : 0;
}

static const char* leaf_name(const char* path) {
    const char* p = strrchr(path, '/');
    const char* q = strrchr(path, '\\');
    const char* r = p > q ? p : q;
    return r ? r + 1 : path;
}

static void path_native(char* p) {
    while (*p) {
        if (*p == '/') *p = '\\';
        p++;
    }
}

static void path_unix(char* p) {
    while (*p) {
        if (*p == '\\') *p = '/';
        p++;
    }
}

static int mkdir_recursive(const char* path) {
    char tmp[1024];
    char* p;
    size_t len;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    if (tmp[len - 1] == '\\' || tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            if (*tmp) mkdir_p(tmp);
            *p = '\\';
        }
    }
    if (*tmp) mkdir_p(tmp);
    return 0;
}

static int mkdir_parent(const char* path) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    /* Remove trailing separator if present */
    size_t len = strlen(tmp);
    while (len > 0 && (tmp[len-1] == '\\' || tmp[len-1] == '/'))
        tmp[--len] = '\0';
    /* Find last separator */
    char* last = strrchr(tmp, '\\');
    char* last2 = strrchr(tmp, '/');
    char* sep = (last > last2) ? last : last2;
    if (sep) {
        *sep = '\0';
        return mkdir_recursive(tmp);
    }
    return 0;  /* no parent dir needed */
}

static int file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & S_IFDIR) != 0;
}

static unsigned long crc32_file(FILE* f) {
    unsigned long crc = crc32(0L, Z_NULL, 0);
    unsigned char buf[65536];
    size_t n;
    rewind(f);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc = crc32(crc, buf, (unsigned int)n);
    return crc;
}

int l1z_detect_format(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return L1Z_FORMAT_UNKNOWN;

    char low[32];
    int i;
    ext++;
    for (i = 0; ext[i] && i < 31; i++)
        low[i] = (ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i];
    low[i] = '\0';

    if (strcmp(low, "zip") == 0) return L1Z_FORMAT_ZIP;
    if (strcmp(low, "1z") == 0) return L1Z_FORMAT_1Z;
    if (strcmp(low, "1zip") == 0) return L1Z_FORMAT_1ZIP;
    if (strcmp(low, "af") == 0) return L1Z_FORMAT_AF;
    if (strcmp(low, "ez") == 0) return L1Z_FORMAT_EZ;
    if (strcmp(low, "hiz") == 0) return L1Z_FORMAT_HIZ;
    if (strcmp(low, "7z") == 0) return L1Z_FORMAT_7Z;
    if (strcmp(low, "rar") == 0) return L1Z_FORMAT_RAR;
    if (strcmp(low, "iso") == 0) return L1Z_FORMAT_ISO;
    if (strcmp(low, "img") == 0) return L1Z_FORMAT_IMG;

    FILE* f = fopen(path, "rb");
    if (f) {
        unsigned char magic[8];
        size_t n = fread(magic, 1, 8, f);
        fclose(f);
        if (n >= 2) {
            if (magic[0] == '1' && magic[1] == 'Z') return L1Z_FORMAT_1Z;
            if (magic[0] == 'A' && magic[1] == 'F') return L1Z_FORMAT_AF;
        }
        if (n >= 4) {
            if (magic[0] == 0x50 && magic[1] == 0x4B && magic[2] == 0x03 && magic[3] == 0x04)
                return L1Z_FORMAT_ZIP;
            if (magic[0] == 0x37 && magic[1] == 0x7A) return L1Z_FORMAT_7Z;
            if (magic[0] == 0x52 && magic[1] == 0x61 && magic[2] == 0x72) return L1Z_FORMAT_RAR;
        }
        if (n >= 5 && memcmp(magic, "\x43\x44\x30\x30\x31", 5) == 0) return L1Z_FORMAT_ISO;
    }

    return L1Z_FORMAT_UNKNOWN;
}

static const char* format_name(int fmt) {
    switch (fmt) {
        case L1Z_FORMAT_ZIP:  return "ZIP";
        case L1Z_FORMAT_1Z:   return "1Z";
        case L1Z_FORMAT_1ZIP: return "1ZIP";
        case L1Z_FORMAT_AF:   return "AF";
        case L1Z_FORMAT_EZ:   return "EZ";
        case L1Z_FORMAT_HIZ:  return "HIZ";
        case L1Z_FORMAT_7Z:   return "7Z";
        case L1Z_FORMAT_RAR:  return "RAR";
        case L1Z_FORMAT_ISO:  return "ISO";
        case L1Z_FORMAT_IMG:  return "IMG";
        default:              return "UNKNOWN";
    }
}

const char* l1z_strerror(int code) {
    switch (code) {
        case L1Z_OK:           return "Success";
        case L1Z_E_OPEN:       return "Cannot open archive";
        case L1Z_E_READ:       return "Read error";
        case L1Z_E_WRITE:      return "Write error";
        case L1Z_E_MEMORY:     return "Out of memory";
        case L1Z_E_FORMAT:     return "Unsupported or corrupted format";
        case L1Z_E_COMPRESS:   return "Compression error";
        case L1Z_E_DECOMPRESS: return "Decompression error";
        case L1Z_E_NOTFOUND:   return "File not found in archive";
        case L1Z_E_EXISTS:     return "File already exists";
        case L1Z_E_PASSWORD:   return "Wrong password or not encrypted";
        case L1Z_E_UNSUPPORTED: return "Format not yet supported";
        case L1Z_E_PARAM:      return "Invalid parameter";
        case L1Z_E_CREATE:     return "Cannot create output file";
        case L1Z_E_CRC:        return "CRC check failed";
        default:               return "Unknown error";
    }
}

/* ========================================================================
 * ZIP format implementation (using zlib for deflate/inflate)
 * ======================================================================== */

static int zip_write_local_header(FILE* f, const char* name, unsigned long crc,
                                   unsigned int comp_size, unsigned int uncomp_size,
                                   int method, unsigned short dos_time, unsigned short dos_date)
{
    ZipLocalHeader hdr;
    unsigned short nlen = (unsigned short)strlen(name);
    memset(&hdr, 0, sizeof(hdr));
    hdr.sig = ZIP_LOCAL_HEADER_SIG;
    hdr.version_needed = 20;
    hdr.method = (unsigned short)method;
    hdr.mod_time = dos_time;
    hdr.mod_date = dos_date;
    hdr.crc32 = crc;
    hdr.comp_size = comp_size;
    hdr.uncomp_size = uncomp_size;
    hdr.filename_len = nlen;
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) return L1Z_E_WRITE;
    if (fwrite(name, 1, nlen, f) != nlen) return L1Z_E_WRITE;
    return L1Z_OK;
}

static int zip_write_central_entry(FILE* f, const char* name, unsigned long crc,
                                    unsigned int comp_size, unsigned int uncomp_size,
                                    int method, unsigned short dos_time,
                                    unsigned short dos_date, unsigned int offset,
                                    int is_dir)
{
    ZipCentralHeader hdr;
    unsigned short nlen = (unsigned short)strlen(name);
    memset(&hdr, 0, sizeof(hdr));
    hdr.sig = ZIP_CENTRAL_SIG;
    hdr.made_by = 20;
    hdr.version_needed = 20;
    hdr.method = (unsigned short)method;
    hdr.mod_time = dos_time;
    hdr.mod_date = dos_date;
    hdr.crc32 = crc;
    hdr.comp_size = comp_size;
    hdr.uncomp_size = uncomp_size;
    hdr.filename_len = nlen;
    hdr.offset = offset;
    if (is_dir) hdr.external_attr = (S_IFDIR | 0755) << 16;
    else        hdr.external_attr = (S_IFREG | 0644) << 16;
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) return L1Z_E_WRITE;
    if (fwrite(name, 1, nlen, f) != nlen) return L1Z_E_WRITE;
    return L1Z_OK;
}

static int zip_write_end(FILE* f, unsigned int central_offset, unsigned int central_size,
                          unsigned short entries)
{
    ZipEndCentral end;
    memset(&end, 0, sizeof(end));
    end.sig = ZIP_END_SIG;
    end.entries_disk = entries;
    end.entries_total = entries;
    end.central_size = central_size;
    end.central_offset = central_offset;
    if (fwrite(&end, sizeof(end), 1, f) != 1) return L1Z_E_WRITE;
    return L1Z_OK;
}

static unsigned short dos_time_now(void) {
    time_t t = time(NULL);
    struct tm* lt = localtime(&t);
    return (unsigned short)((lt->tm_sec / 2) | (lt->tm_min << 5) | (lt->tm_hour << 11));
}

static unsigned short dos_date_now(void) {
    time_t t = time(NULL);
    struct tm* lt = localtime(&t);
    return (unsigned short)(lt->tm_mday | ((lt->tm_mon + 1) << 5) |
           ((lt->tm_year - 80) << 9));
}

static int zip_compress_file(FILE* fin, FILE* fout, int level,
                              unsigned long* out_crc, unsigned int* out_comp,
                              unsigned int* out_uncomp)
{
    z_stream strm;
    unsigned char inbuf[65536];
    unsigned char outbuf[65536];
    int flush, ret;
    unsigned long crc = crc32(0L, Z_NULL, 0);
    unsigned int uncomp = 0;

    memset(&strm, 0, sizeof(strm));
    ret = deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return L1Z_E_COMPRESS;

    rewind(fin);
    do {
        strm.avail_in = (uInt)fread(inbuf, 1, sizeof(inbuf), fin);
        if (ferror(fin)) { deflateEnd(&strm); return L1Z_E_READ; }
        flush = feof(fin) ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = inbuf;
        crc = crc32(crc, inbuf, strm.avail_in);
        uncomp += strm.avail_in;

        do {
            strm.avail_out = sizeof(outbuf);
            strm.next_out = outbuf;
            ret = deflate(&strm, flush);
            if (ret == Z_STREAM_ERROR) { deflateEnd(&strm); return L1Z_E_COMPRESS; }
            unsigned int have = sizeof(outbuf) - strm.avail_out;
            if (fwrite(outbuf, 1, have, fout) != have) { deflateEnd(&strm); return L1Z_E_WRITE; }
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);

    *out_comp = (unsigned int)strm.total_out;
    *out_uncomp = uncomp;
    *out_crc = crc;
    deflateEnd(&strm);
    return L1Z_OK;
}

static int zip_decompress_data(const unsigned char* in, unsigned int inlen,
                                unsigned char* out, unsigned int* outlen)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int ret = inflateInit2(&strm, -15);
    if (ret != Z_OK) return L1Z_E_DECOMPRESS;

    strm.next_in = (unsigned char*)in;
    strm.avail_in = inlen;
    strm.next_out = out;
    strm.avail_out = *outlen;

    ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) { inflateEnd(&strm); return L1Z_E_DECOMPRESS; }
    *outlen = (unsigned int)strm.total_out;
    inflateEnd(&strm);
    return L1Z_OK;
}

/* ZIP read: scan central directory and build entry list */
static int zip_read_central_dir(L1Z_Archive* arc) {
    FILE* f = arc->fp;
    ZipEndCentral end;
    unsigned char buf[65536];
    long fsize;
    int i;

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);

    long search = fsize - 65536;
    if (search < 0) search = 0;
    fseek(f, search, SEEK_SET);
    unsigned int nread = (unsigned int)fread(buf, 1, (size_t)(fsize - search), f);
    if (nread < sizeof(ZipEndCentral)) return L1Z_E_FORMAT;

    int found = 0;
    int pos;
    for (pos = (int)nread - (int)sizeof(ZipEndCentral); pos >= 0; pos--) {
        unsigned int* p = (unsigned int*)(buf + pos);
        if (*p == ZIP_END_SIG) {
            memcpy(&end, buf + pos, sizeof(end));
            found = 1;
            break;
        }
    }
    if (!found) {
        /* try at very end */
        fseek(f, fsize - (long)sizeof(ZipEndCentral), SEEK_SET);
        if (fread(&end, 1, sizeof(end), f) != sizeof(end)) return L1Z_E_FORMAT;
        if (end.sig != ZIP_END_SIG) return L1Z_E_FORMAT;
    }

    if (end.entries_total == 0) return L1Z_OK;

    arc->entry_count = end.entries_total;
    arc->entries = (L1Z_Entry*)calloc(arc->entry_count, sizeof(L1Z_Entry));
    if (!arc->entries) return L1Z_E_MEMORY;

    fseek(f, end.central_offset, SEEK_SET);
    for (i = 0; i < arc->entry_count; i++) {
        ZipCentralHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) { free(arc->entries); arc->entries = NULL; arc->entry_count = 0; return L1Z_E_FORMAT; }
        if (ch.sig != ZIP_CENTRAL_SIG) { free(arc->entries); arc->entries = NULL; arc->entry_count = 0; return L1Z_E_FORMAT; }

        unsigned short fnlen = ch.filename_len;
        if (fnlen >= sizeof(arc->entries[i].filename)) fnlen = sizeof(arc->entries[i].filename) - 1;
        if (fread(arc->entries[i].filename, 1, fnlen, f) != fnlen) { free(arc->entries); arc->entries = NULL; arc->entry_count = 0; return L1Z_E_FORMAT; }
        arc->entries[i].filename[fnlen] = '\0';
        path_unix(arc->entries[i].filename);

        arc->entries[i].uncompressed_size = ch.uncomp_size;
        arc->entries[i].compressed_size = ch.comp_size;
        arc->entries[i].crc32 = ch.crc32;
        arc->entries[i].is_dir = (ch.uncomp_size == 0 && fnlen > 0 && arc->entries[i].filename[fnlen - 1] == '/');
        arc->entries[i].index = i;
        arc->entries[i].local_offset = ch.offset;

        if (ch.extra_len) fseek(f, ch.extra_len, SEEK_CUR);
        if (ch.comment_len) fseek(f, ch.comment_len, SEEK_CUR);
    }

    return L1Z_OK;
}

/* Add a pending entry for ZIP finalization */
static int zip_add_pending(L1Z_Archive* arc, const char* name,
                            unsigned long crc, unsigned int comp_size,
                            unsigned int uncomp_size, int method,
                            unsigned short dos_t, unsigned short dos_d,
                            long offset)
{
    if (arc->zip_pending_count >= arc->zip_pending_cap) {
        int newcap = arc->zip_pending_cap ? arc->zip_pending_cap * 2 : 32;
        ZipPendingEntry* tmp = (ZipPendingEntry*)realloc(arc->zip_pending,
                                (size_t)newcap * sizeof(ZipPendingEntry));
        if (!tmp) return L1Z_E_MEMORY;
        arc->zip_pending = tmp;
        arc->zip_pending_cap = newcap;
    }
    ZipPendingEntry* e = &arc->zip_pending[arc->zip_pending_count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->crc32 = crc;
    e->comp_size = comp_size;
    e->uncomp_size = uncomp_size;
    e->method = method;
    e->dos_time = dos_t;
    e->dos_date = dos_d;
    e->header_offset = offset;
    return L1Z_OK;
}

/* Compress a file into a ZIP archive (appending) */
static int zip_add_file(L1Z_Archive* arc, const char* filepath, int level) {
    FILE* fin = fopen(filepath, "rb");
    if (!fin) return L1Z_E_OPEN;

    const char* fname = leaf_name(filepath);
    unsigned short dos_t = dos_time_now();
    unsigned short dos_d = dos_date_now();
    int method = (level == 0) ? 0 : 8;

    /* Read the entire file into memory */
    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    rewind(fin);

    unsigned char* data = (unsigned char*)malloc(fsize > 0 ? (size_t)fsize : 1);
    if (!data) { fclose(fin); return L1Z_E_MEMORY; }
    size_t nread = fread(data, 1, (size_t)fsize, fin);
    fclose(fin);
    if ((long)nread != fsize) { free(data); return L1Z_E_READ; }

    unsigned long crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data, (unsigned int)fsize);

    /* Compress to memory buffer */
    unsigned long bound = deflateBound(0, (unsigned long)fsize);
    unsigned char* comp = (unsigned char*)malloc(bound);
    if (!comp) { free(data); return L1Z_E_MEMORY; }
    unsigned long comp_size = 0;

    if (method == 8) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        int ret = deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) { free(data); free(comp); return L1Z_E_COMPRESS; }
        strm.next_in = data;
        strm.avail_in = (unsigned int)fsize;
        strm.next_out = comp;
        strm.avail_out = (uInt)bound;
        ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) { deflateEnd(&strm); free(data); free(comp); return L1Z_E_COMPRESS; }
        comp_size = strm.total_out;
        deflateEnd(&strm);
    } else {
        memcpy(comp, data, (size_t)fsize);
        comp_size = (unsigned long)fsize;
    }

    /* Write local file header at current position */
    long header_offset = ftell(arc->fp);
    int ret = zip_write_local_header(arc->fp, fname, crc, (unsigned int)comp_size,
                                      (unsigned int)fsize, method, dos_t, dos_d);
    if (ret != L1Z_OK) { free(data); free(comp); return ret; }

    /* Write compressed data after header */
    if (fwrite(comp, 1, (size_t)comp_size, arc->fp) != (size_t)comp_size) {
        free(data); free(comp); return L1Z_E_WRITE;
    }

    /* Track entry for finalization */
    ret = zip_add_pending(arc, fname, crc, (unsigned int)comp_size,
                           (unsigned int)fsize, method, dos_t, dos_d, header_offset);

    free(data);
    free(comp);
    arc->modified = 1;
    return ret;
}

/* Extract a stored/deflated entry from ZIP */
static int zip_extract_entry(L1Z_Archive* arc, int idx, const char* outdir,
                              int preserve_paths, const char* password, int overwrite)
{
    (void)password;
    if (idx < 0 || idx >= arc->entry_count) return L1Z_E_NOTFOUND;
    L1Z_Entry* entry = &arc->entries[idx];

    /* Build output path */
    char outpath[1024];
    if (preserve_paths) {
        snprintf(outpath, sizeof(outpath), "%s/%s", outdir, entry->filename);
    } else {
        snprintf(outpath, sizeof(outpath), "%s/%s", outdir, leaf_name(entry->filename));
    }
    path_native(outpath);

    if (entry->is_dir) {
        mkdir_recursive(outpath);
        return L1Z_OK;
    }

    if (!overwrite && file_exists(outpath)) return L1Z_E_EXISTS;

    mkdir_parent(outpath);

    FILE* f = arc->fp;
    unsigned int hdr_offset = entry->local_offset;

    fseek(f, hdr_offset, SEEK_SET);
    ZipLocalHeader lh;
    if (fread(&lh, sizeof(lh), 1, f) != 1) return L1Z_E_FORMAT;
    if (lh.sig != ZIP_LOCAL_HEADER_SIG) return L1Z_E_FORMAT;

    fseek(f, hdr_offset + ZIP_LOCAL_HEADER_SIZE + lh.filename_len + lh.extra_len, SEEK_SET);

    unsigned int comp_size = lh.comp_size;
    unsigned int uncomp_size = lh.uncomp_size;

    unsigned char* comp_data = (unsigned char*)malloc(comp_size ? comp_size : 1);
    if (!comp_data) return L1Z_E_MEMORY;
    unsigned char* uncomp_data = (unsigned char*)malloc(uncomp_size ? uncomp_size : 1);
    if (!uncomp_data) { free(comp_data); return L1Z_E_MEMORY; }

    if (fread(comp_data, 1, comp_size, f) != comp_size) { free(comp_data); free(uncomp_data); return L1Z_E_READ; }

    int ret = L1Z_OK;
    if (lh.method == 0) {
        memcpy(uncomp_data, comp_data, uncomp_size);
    } else if (lh.method == 8) {
        unsigned int dec_len = uncomp_size;
        ret = zip_decompress_data(comp_data, comp_size, uncomp_data, &dec_len);
        if (ret != L1Z_OK) { free(comp_data); free(uncomp_data); return ret; }
    } else {
        free(comp_data); free(uncomp_data);
        return L1Z_E_UNSUPPORTED;
    }

    unsigned long calc_crc = crc32(0L, Z_NULL, 0);
    calc_crc = crc32(calc_crc, uncomp_data, uncomp_size);
    if (calc_crc != lh.crc32) { free(comp_data); free(uncomp_data); return L1Z_E_CRC; }

    FILE* fout = fopen(outpath, "wb");
    if (!fout) { free(comp_data); free(uncomp_data); return L1Z_E_CREATE; }
    if (fwrite(uncomp_data, 1, uncomp_size, fout) != uncomp_size) {
        fclose(fout); free(comp_data); free(uncomp_data); return L1Z_E_WRITE;
    }
    fclose(fout);

    free(comp_data);
    free(uncomp_data);
    return L1Z_OK;
}

/* ZIP read all entries for listing */
static int zip_list(L1Z_Archive* arc, L1Z_Entry** entries, int* count) {
    if (!arc->entries) {
        int ret = zip_read_central_dir(arc);
        if (ret != L1Z_OK) return ret;
    }
    *entries = arc->entries;
    *count = arc->entry_count;
    return L1Z_OK;
}

/* ZIP test entry */
static int zip_test_entry(L1Z_Archive* arc, int idx, const char* password) {
    (void)password;
    if (idx < 0 || idx >= arc->entry_count) return L1Z_E_NOTFOUND;
    L1Z_Entry* entry = &arc->entries[idx];
    if (entry->is_dir) return L1Z_OK;

    FILE* f = arc->fp;
    unsigned int hdr_offset = entry->local_offset;

    fseek(f, hdr_offset, SEEK_SET);
    ZipLocalHeader lh;
    if (fread(&lh, sizeof(lh), 1, f) != 1) return L1Z_E_FORMAT;

    fseek(f, hdr_offset + ZIP_LOCAL_HEADER_SIZE + lh.filename_len + lh.extra_len, SEEK_SET);

    unsigned char* comp_data = (unsigned char*)malloc(lh.comp_size ? lh.comp_size : 1);
    if (!comp_data) return L1Z_E_MEMORY;
    unsigned char* uncomp_data = (unsigned char*)malloc(lh.uncomp_size ? lh.uncomp_size : 1);
    if (!uncomp_data) { free(comp_data); return L1Z_E_MEMORY; }

    if (fread(comp_data, 1, lh.comp_size, f) != lh.comp_size) { free(comp_data); free(uncomp_data); return L1Z_E_READ; }

    int ret = L1Z_OK;
    if (lh.method == 0) {
        memcpy(uncomp_data, comp_data, lh.uncomp_size);
    } else if (lh.method == 8) {
        unsigned int dec_len = lh.uncomp_size;
        ret = zip_decompress_data(comp_data, lh.comp_size, uncomp_data, &dec_len);
        if (ret != L1Z_OK) { free(comp_data); free(uncomp_data); return ret; }
    } else {
        free(comp_data); free(uncomp_data);
        return L1Z_E_UNSUPPORTED;
    }

    unsigned long calc_crc = crc32(0L, Z_NULL, 0);
    calc_crc = crc32(calc_crc, uncomp_data, lh.uncomp_size);
    if (calc_crc != lh.crc32) { free(comp_data); free(uncomp_data); return L1Z_E_CRC; }

    free(comp_data); free(uncomp_data);
    return L1Z_OK;
}

/* ========================================================================
 * 1Z format implementation
 * ======================================================================== */

static int f1z_create(L1Z_Archive* arc) {
    unsigned char hdr[12] = { '1', 'Z', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (fwrite(hdr, 1, 12, arc->fp) != 12) return L1Z_E_WRITE;
    return L1Z_OK;
}

static int f1z_read_index(L1Z_Archive* arc) {
    FILE* f = arc->fp;
    rewind(f);
    unsigned char hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return L1Z_E_FORMAT;
    if (hdr[0] != '1' || hdr[1] != 'Z') return L1Z_E_FORMAT;
    if (hdr[2] != 1) return L1Z_E_FORMAT;

    /* free existing entries first */
    if (arc->entries) { free(arc->entries); arc->entries = NULL; }
    arc->entry_count = 0;

    unsigned int count;
    if (fread(&count, 4, 1, f) != 1) return L1Z_E_FORMAT;

    arc->entry_count = (int)count;
    arc->entries = (L1Z_Entry*)calloc(count, sizeof(L1Z_Entry));
    if (!arc->entries) return L1Z_E_MEMORY;

    for (unsigned int i = 0; i < count; i++) {
        unsigned short fnlen;
        if (fread(&fnlen, 2, 1, f) != 1) { free(arc->entries); return L1Z_E_FORMAT; }
        unsigned short rlen = fnlen < 1023 ? fnlen : 1023;
        if (fread(arc->entries[i].filename, 1, rlen, f) != rlen) { free(arc->entries); return L1Z_E_FORMAT; }
        arc->entries[i].filename[rlen] = '\0';

        if (fread(&arc->entries[i].uncompressed_size, 8, 1, f) != 1 ||
            fread(&arc->entries[i].compressed_size, 8, 1, f) != 1 ||
            fread(&arc->entries[i].crc32, 4, 1, f) != 1) { free(arc->entries); return L1Z_E_FORMAT; }

        unsigned char flags;
        if (fread(&flags, 1, 1, f) != 1) { free(arc->entries); return L1Z_E_FORMAT; }
        arc->entries[i].is_dir = (flags & 1) ? 1 : 0;
        arc->entries[i].index = (int)i;

        /* Skip compressed data */
        fseek(f, (long)arc->entries[i].compressed_size, SEEK_CUR);
    }

    return L1Z_OK;
}

static int f1z_add(L1Z_Archive* arc, const char* filepath, int level) {
    FILE* fin = fopen(filepath, "rb");
    if (!fin) return L1Z_E_OPEN;

    const char* fname = leaf_name(filepath);

    fseek(fin, 0, SEEK_END);
    unsigned long long fsize = ftell(fin);
    rewind(fin);

    unsigned char* data = (unsigned char*)malloc(fsize ? (size_t)fsize : 1);
    if (!data) { fclose(fin); return L1Z_E_MEMORY; }
    if (fread(data, 1, (size_t)fsize, fin) != (size_t)fsize) { fclose(fin); free(data); return L1Z_E_READ; }
    fclose(fin);

    unsigned long crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data, (unsigned int)fsize);

    /* Compress */
    unsigned long comp_bound = deflateBound(0, (unsigned long)fsize);
    unsigned char* comp_data = (unsigned char*)malloc(comp_bound);
    if (!comp_data) { free(data); return L1Z_E_MEMORY; }

    unsigned long comp_size = 0;
    int use_compression = (level > 0);
    if (use_compression) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { free(data); free(comp_data); return L1Z_E_COMPRESS; }
        strm.next_in = data;
        strm.avail_in = (unsigned int)fsize;
        strm.next_out = comp_data;
        strm.avail_out = (uInt)comp_bound;
        int ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) { deflateEnd(&strm); free(data); free(comp_data); return L1Z_E_COMPRESS; }
        comp_size = strm.total_out;
        deflateEnd(&strm);
    } else {
        memcpy(comp_data, data, (size_t)fsize);
        comp_size = (unsigned long)fsize;
    }

    /* Write entry: filename_len(2) + filename + uncomp_size(8) + comp_size(8) + crc32(4) + flags(1) + data */
    unsigned short fnlen = (unsigned short)strlen(fname);
    if (fwrite(&fnlen, 2, 1, arc->fp) != 1) { free(data); free(comp_data); return L1Z_E_WRITE; }
    if (fwrite(fname, 1, fnlen, arc->fp) != fnlen) { free(data); free(comp_data); return L1Z_E_WRITE; }
    if (fwrite(&fsize, 8, 1, arc->fp) != 1) { free(data); free(comp_data); return L1Z_E_WRITE; }
    unsigned long long csize = comp_size;
    if (fwrite(&csize, 8, 1, arc->fp) != 1) { free(data); free(comp_data); return L1Z_E_WRITE; }
    if (fwrite(&crc, 4, 1, arc->fp) != 1) { free(data); free(comp_data); return L1Z_E_WRITE; }
    unsigned char flags = 0;
    if (use_compression) flags |= 2;
    if (fwrite(&flags, 1, 1, arc->fp) != 1) { free(data); free(comp_data); return L1Z_E_WRITE; }
    if (fwrite(comp_data, 1, (size_t)comp_size, arc->fp) != (size_t)comp_size) { free(data); free(comp_data); return L1Z_E_WRITE; }

    free(data);
    free(comp_data);

    arc->modified = 1;
    arc->entry_count++;
    return L1Z_OK;
}

static int f1z_extract_entry(L1Z_Archive* arc, int idx, const char* outdir,
                              int preserve_paths, const char* password, int overwrite)
{
    (void)password;
    if (idx < 0 || idx >= arc->entry_count) return L1Z_E_NOTFOUND;
    L1Z_Entry* entry = &arc->entries[idx];

    char outpath[1024];
    if (preserve_paths)
        snprintf(outpath, sizeof(outpath), "%s/%s", outdir, entry->filename);
    else
        snprintf(outpath, sizeof(outpath), "%s/%s", outdir, leaf_name(entry->filename));
    path_native(outpath);

    if (entry->is_dir) { mkdir_recursive(outpath); return L1Z_OK; }
    if (!overwrite && file_exists(outpath)) return L1Z_E_EXISTS;
    mkdir_parent(outpath);

    /* Seek to data for this entry */
    FILE* f = arc->fp;
    rewind(f);
    fseek(f, 8, SEEK_SET); /* skip header */
    unsigned int count;
    fread(&count, 4, 1, f);

    /* Skip entries before idx */
    for (int i = 0; i < idx && i < (int)count; i++) {
        unsigned short fnlen;
        if (fread(&fnlen, 2, 1, f) != 1) return L1Z_E_FORMAT;
        fseek(f, fnlen, SEEK_CUR); /* skip filename */
        unsigned long long us, cs;
        if (fread(&us, 8, 1, f) != 1) return L1Z_E_FORMAT;
        if (fread(&cs, 8, 1, f) != 1) return L1Z_E_FORMAT;
        fseek(f, 4, SEEK_CUR); /* skip crc32 */
        fseek(f, 1, SEEK_CUR); /* skip flags */
        fseek(f, (long)cs, SEEK_CUR); /* skip data */
    }

    unsigned short fnlen;
    if (fread(&fnlen, 2, 1, f) != 1) return L1Z_E_FORMAT;
    fseek(f, fnlen, SEEK_CUR);
    unsigned long long us, cs;
    if (fread(&us, 8, 1, f) != 1) return L1Z_E_FORMAT;
    if (fread(&cs, 8, 1, f) != 1) return L1Z_E_FORMAT;
    unsigned long stored_crc;
    if (fread(&stored_crc, 4, 1, f) != 1) return L1Z_E_FORMAT;
    unsigned char flags;
    if (fread(&flags, 1, 1, f) != 1) return L1Z_E_FORMAT;

    unsigned char* comp_data = (unsigned char*)malloc((size_t)cs ? (size_t)cs : 1);
    if (!comp_data) return L1Z_E_MEMORY;
    if (fread(comp_data, 1, (size_t)cs, f) != (size_t)cs) { free(comp_data); return L1Z_E_READ; }

    unsigned char* uncomp_data = (unsigned char*)malloc((size_t)us ? (size_t)us : 1);
    if (!uncomp_data) { free(comp_data); return L1Z_E_MEMORY; }

    int ret = L1Z_OK;
    if (flags & 2) {
        /* compressed with deflate */
        unsigned int dec_len = (unsigned int)us;
        ret = zip_decompress_data(comp_data, (unsigned int)cs, uncomp_data, &dec_len);
        if (ret != L1Z_OK) { free(comp_data); free(uncomp_data); return ret; }
    } else {
        memcpy(uncomp_data, comp_data, (size_t)us);
    }

    unsigned long calc_crc = crc32(0L, Z_NULL, 0);
    calc_crc = crc32(calc_crc, uncomp_data, (unsigned int)us);
    if (calc_crc != stored_crc) { free(comp_data); free(uncomp_data); return L1Z_E_CRC; }

    FILE* fout = fopen(outpath, "wb");
    if (!fout) { free(comp_data); free(uncomp_data); return L1Z_E_CREATE; }
    if (fwrite(uncomp_data, 1, (size_t)us, fout) != (size_t)us) { fclose(fout); free(comp_data); free(uncomp_data); return L1Z_E_WRITE; }
    fclose(fout);

    free(comp_data);
    free(uncomp_data);
    return L1Z_OK;
}

/* ========================================================================
 * AF format implementation (no compression)
 * ======================================================================== */

static int af_create(L1Z_Archive* arc) {
    unsigned char hdr[8] = { 'A', 'F', 1, 0, 0, 0, 0, 0 };
    if (fwrite(hdr, 1, 8, arc->fp) != 8) return L1Z_E_WRITE;
    return L1Z_OK;
}

static int af_read_index(L1Z_Archive* arc) {
    FILE* f = arc->fp;
    rewind(f);
    unsigned char hdr[4];
    if (fread(hdr, 1, 4, f) != 4) return L1Z_E_FORMAT;
    if (hdr[0] != 'A' || hdr[1] != 'F') return L1Z_E_FORMAT;
    if (hdr[2] != 1) return L1Z_E_FORMAT;

    if (arc->entries) { free(arc->entries); arc->entries = NULL; }
    arc->entry_count = 0;

    unsigned int count;
    if (fread(&count, 4, 1, f) != 1) return L1Z_E_FORMAT;

    arc->entry_count = (int)count;
    arc->entries = (L1Z_Entry*)calloc(count, sizeof(L1Z_Entry));
    if (!arc->entries) return L1Z_E_MEMORY;

    for (unsigned int i = 0; i < count; i++) {
        unsigned short fnlen;
        if (fread(&fnlen, 2, 1, f) != 1) { free(arc->entries); return L1Z_E_FORMAT; }
        unsigned short rlen = fnlen < 1023 ? fnlen : 1023;
        if (fread(arc->entries[i].filename, 1, rlen, f) != rlen) { free(arc->entries); return L1Z_E_FORMAT; }
        arc->entries[i].filename[rlen] = '\0';

        if (fread(&arc->entries[i].uncompressed_size, 8, 1, f) != 1) { free(arc->entries); return L1Z_E_FORMAT; }
        arc->entries[i].compressed_size = arc->entries[i].uncompressed_size;
        arc->entries[i].crc32 = 0;
        arc->entries[i].is_dir = 0;
        arc->entries[i].index = (int)i;

        fseek(f, (long)arc->entries[i].uncompressed_size, SEEK_CUR);
    }

    return L1Z_OK;
}

static int af_add(L1Z_Archive* arc, const char* filepath, int level) {
    (void)level;
    FILE* fin = fopen(filepath, "rb");
    if (!fin) return L1Z_E_OPEN;

    const char* fname = leaf_name(filepath);

    fseek(fin, 0, SEEK_END);
    unsigned long long fsize = ftell(fin);
    rewind(fin);

    unsigned short fnlen = (unsigned short)strlen(fname);
    if (fwrite(&fnlen, 2, 1, arc->fp) != 1) { fclose(fin); return L1Z_E_WRITE; }
    if (fwrite(fname, 1, fnlen, arc->fp) != fnlen) { fclose(fin); return L1Z_E_WRITE; }
    if (fwrite(&fsize, 8, 1, arc->fp) != 1) { fclose(fin); return L1Z_E_WRITE; }

    /* Copy raw data */
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, arc->fp) != n) { fclose(fin); return L1Z_E_WRITE; }
    }
    fclose(fin);

    arc->modified = 1;
    arc->entry_count++;
    return L1Z_OK;
}

static int af_extract_entry(L1Z_Archive* arc, int idx, const char* outdir,
                             int preserve_paths, const char* password, int overwrite)
{
    (void)password;
    if (idx < 0 || idx >= arc->entry_count) return L1Z_E_NOTFOUND;
    L1Z_Entry* entry = &arc->entries[idx];

    char outpath[1024];
    if (preserve_paths)
        snprintf(outpath, sizeof(outpath), "%s/%s", outdir, entry->filename);
    else
        snprintf(outpath, sizeof(outpath), "%s/%s", outdir, leaf_name(entry->filename));
    path_native(outpath);

    if (entry->is_dir) { mkdir_recursive(outpath); return L1Z_OK; }
    if (!overwrite && file_exists(outpath)) return L1Z_E_EXISTS;
    mkdir_parent(outpath);

    FILE* f = arc->fp;
    rewind(f);
    fseek(f, 4, SEEK_SET);
    unsigned int count;
    fread(&count, 4, 1, f);

    for (int i = 0; i < idx && i < (int)count; i++) {
        unsigned short fnlen;
        if (fread(&fnlen, 2, 1, f) != 1) return L1Z_E_FORMAT;
        fseek(f, fnlen, SEEK_CUR);
        unsigned long long sz;
        if (fread(&sz, 8, 1, f) != 1) return L1Z_E_FORMAT;
        fseek(f, (long)sz, SEEK_CUR);
    }

    unsigned short fnlen;
    if (fread(&fnlen, 2, 1, f) != 1) return L1Z_E_FORMAT;
    fseek(f, fnlen, SEEK_CUR);
    unsigned long long sz;
    if (fread(&sz, 8, 1, f) != 1) return L1Z_E_FORMAT;

    FILE* fout = fopen(outpath, "wb");
    if (!fout) return L1Z_E_CREATE;

    unsigned char buf[65536];
    unsigned long long remaining = sz;
    while (remaining > 0) {
        size_t toread = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        size_t n = fread(buf, 1, toread, f);
        if (n == 0) break;
        if (fwrite(buf, 1, n, fout) != n) { fclose(fout); return L1Z_E_WRITE; }
        remaining -= n;
    }
    fclose(fout);
    return L1Z_OK;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

L1Z_Archive* l1z_open(const char* path, const char* mode, int format) {
    L1Z_Archive* arc = (L1Z_Archive*)calloc(1, sizeof(L1Z_Archive));
    if (!arc) return NULL;

    strncpy(arc->path, path, sizeof(arc->path) - 1);
    strncpy(arc->mode, mode, sizeof(arc->mode) - 1);

    if (format == L1Z_FORMAT_UNKNOWN)
        format = l1z_detect_format(path);
    arc->format = format;

    int create_new = 0;
    if (strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0) {
        arc->fp = fopen(path, "wb");
        create_new = 1;
    } else if (strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0) {
        arc->fp = fopen(path, "rb");
    } else if (strcmp(mode, "r+") == 0 || strcmp(mode, "r+b") == 0) {
        arc->fp = fopen(path, "r+b");
        if (!arc->fp) {
            arc->fp = fopen(path, "wb");
            create_new = 1;
        }
    } else {
        arc->fp = fopen(path, "rb");
    }

    if (!arc->fp) {
        free(arc);
        return NULL;
    }

    if (create_new) {
        int ret = L1Z_OK;
        switch (arc->format) {
            case L1Z_FORMAT_1Z:
            case L1Z_FORMAT_1ZIP:
                ret = f1z_create(arc);
                break;
            case L1Z_FORMAT_AF:
                ret = af_create(arc);
                break;
            case L1Z_FORMAT_ZIP:
            case L1Z_FORMAT_EZ:
            case L1Z_FORMAT_HIZ:
                /* ZIP: just open, header will be written on close */
                break;
            default:
                break;
        }
        if (ret != L1Z_OK) {
            fclose(arc->fp);
            free(arc);
            return NULL;
        }
        arc->entry_count = 0;
        arc->entries = NULL;
    } else {
        /* Read existing index */
        int ret = L1Z_OK;
        switch (arc->format) {
            case L1Z_FORMAT_ZIP:
            case L1Z_FORMAT_EZ:
            case L1Z_FORMAT_HIZ:
                ret = zip_read_central_dir(arc);
                break;
            case L1Z_FORMAT_1Z:
            case L1Z_FORMAT_1ZIP:
                ret = f1z_read_index(arc);
                break;
            case L1Z_FORMAT_AF:
                ret = af_read_index(arc);
                break;
            case L1Z_FORMAT_7Z:
            case L1Z_FORMAT_RAR:
            case L1Z_FORMAT_ISO:
            case L1Z_FORMAT_IMG:
                /* Not fully implemented, return empty entries */
                arc->entry_count = 0;
                arc->entries = NULL;
                break;
            default:
                fclose(arc->fp);
                free(arc);
                return NULL;
        }
        if (ret != L1Z_OK && ret != L1Z_E_FORMAT) {
            /* If format error on read, we can still try to use it */
            arc->entry_count = 0;
            arc->entries = NULL;
        }
    }

    return arc;
}

int l1z_close(L1Z_Archive* arc) {
    if (!arc) return L1Z_E_PARAM;

    if (arc->modified && arc->fp) {
        if (arc->format == L1Z_FORMAT_ZIP || arc->format == L1Z_FORMAT_EZ || arc->format == L1Z_FORMAT_HIZ) {
            /* Write central directory */
            long central_offset = ftell(arc->fp);
            unsigned short entry_count = (unsigned short)arc->zip_pending_count;

            for (int i = 0; i < arc->zip_pending_count; i++) {
                ZipPendingEntry* e = &arc->zip_pending[i];
                zip_write_central_entry(arc->fp, e->name, e->crc32,
                                        e->comp_size, e->uncomp_size,
                                        e->method, e->dos_time, e->dos_date,
                                        (unsigned int)e->header_offset, 0);
            }

            long central_end = ftell(arc->fp);
            unsigned int central_size = (unsigned int)(central_end - central_offset);

            zip_write_end(arc->fp, (unsigned int)central_offset, central_size, entry_count);
        } else if (arc->format == L1Z_FORMAT_1Z || arc->format == L1Z_FORMAT_1ZIP) {
            /* Update file count in header */
            fflush(arc->fp);
            unsigned int count = (unsigned int)arc->entry_count;
            fseek(arc->fp, 8, SEEK_SET);
            fwrite(&count, 4, 1, arc->fp);
        } else if (arc->format == L1Z_FORMAT_AF) {
            /* Update file count in header */
            fflush(arc->fp);
            unsigned int count = (unsigned int)arc->entry_count;
            fseek(arc->fp, 4, SEEK_SET);
            fwrite(&count, 4, 1, arc->fp);
        }
    }

    if (arc->fp) fclose(arc->fp);
    if (arc->entries) free(arc->entries);
    if (arc->zip_pending) free(arc->zip_pending);
    free(arc);
    return L1Z_OK;
}

int l1z_add(L1Z_Archive* arc, const char* filepath, const char* password,
            int level, int recursive, const char* exclude)
{
    (void)password;
    (void)exclude;
    if (!arc || !filepath) return L1Z_E_PARAM;
    if (level < 0) level = 0;
    if (level > 9) level = 9;

    arc->comp_level = level;

    /* If adding a directory, we need recursion */
    int ret = L1Z_OK;
    if (is_directory(filepath)) {
        if (!recursive) return L1Z_OK; /* skip dirs if no recursion */

        DIR* dir = opendir(filepath);
        if (!dir) return L1Z_E_OPEN;

        struct dirent* entry;
        char fullpath[1024];
        char subpath[1024];

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            snprintf(fullpath, sizeof(fullpath), "%s/%s", filepath, entry->d_name);

            if (is_directory(fullpath)) {
                ret = l1z_add(arc, fullpath, password, level, recursive, exclude);
            } else {
                report_progress(entry->d_name, 0);
                switch (arc->format) {
                    case L1Z_FORMAT_ZIP:
                    case L1Z_FORMAT_EZ:
                    case L1Z_FORMAT_HIZ:
                        ret = zip_add_file(arc, fullpath, level);
                        break;
                    case L1Z_FORMAT_1Z:
                    case L1Z_FORMAT_1ZIP:
                        ret = f1z_add(arc, fullpath, level);
                        break;
                    case L1Z_FORMAT_AF:
                        ret = af_add(arc, fullpath, level);
                        break;
                    case L1Z_FORMAT_7Z:
                    case L1Z_FORMAT_RAR:
                    case L1Z_FORMAT_ISO:
                    case L1Z_FORMAT_IMG:
                        ret = L1Z_E_UNSUPPORTED;
                        break;
                    default:
                        ret = L1Z_E_UNSUPPORTED;
                        break;
                }
            }
            if (ret != L1Z_OK && ret != L1Z_E_EXISTS) break;
        }
        closedir(dir);
        return ret;
    }

    /* Single file */
    report_progress(filepath, 0);
    switch (arc->format) {
        case L1Z_FORMAT_ZIP:
        case L1Z_FORMAT_EZ:
        case L1Z_FORMAT_HIZ:
            ret = zip_add_file(arc, filepath, level);
            break;
        case L1Z_FORMAT_1Z:
        case L1Z_FORMAT_1ZIP:
            ret = f1z_add(arc, filepath, level);
            break;
        case L1Z_FORMAT_AF:
            ret = af_add(arc, filepath, level);
            break;
        case L1Z_FORMAT_7Z:
        case L1Z_FORMAT_RAR:
        case L1Z_FORMAT_ISO:
        case L1Z_FORMAT_IMG:
            ret = L1Z_E_UNSUPPORTED;
            break;
        default:
            ret = L1Z_E_UNSUPPORTED;
            break;
    }
    return ret;
}

int l1z_extract(L1Z_Archive* arc, const char* output_dir,
                int preserve_paths, const char* password, int overwrite)
{
    if (!arc || !output_dir) return L1Z_E_PARAM;

    mkdir_recursive(output_dir);

    int ret = L1Z_OK;
    for (int i = 0; i < arc->entry_count; i++) {
        report_progress(arc->entries[i].filename, (i * 100) / (arc->entry_count > 0 ? arc->entry_count : 1));

        switch (arc->format) {
            case L1Z_FORMAT_ZIP:
            case L1Z_FORMAT_EZ:
            case L1Z_FORMAT_HIZ:
                ret = zip_extract_entry(arc, i, output_dir, preserve_paths, password, overwrite);
                break;
            case L1Z_FORMAT_1Z:
            case L1Z_FORMAT_1ZIP:
                ret = f1z_extract_entry(arc, i, output_dir, preserve_paths, password, overwrite);
                break;
            case L1Z_FORMAT_AF:
                ret = af_extract_entry(arc, i, output_dir, preserve_paths, password, overwrite);
                break;
            case L1Z_FORMAT_7Z:
            case L1Z_FORMAT_RAR:
            case L1Z_FORMAT_ISO:
            case L1Z_FORMAT_IMG:
                ret = L1Z_E_UNSUPPORTED;
                break;
            default:
                ret = L1Z_E_UNSUPPORTED;
                break;
        }
        if (ret != L1Z_OK && ret != L1Z_E_EXISTS) break;
    }

    return ret;
}

int l1z_list(L1Z_Archive* arc, L1Z_Entry** entries, int* count) {
    if (!arc || !entries || !count) return L1Z_E_PARAM;

    switch (arc->format) {
        case L1Z_FORMAT_ZIP:
        case L1Z_FORMAT_EZ:
        case L1Z_FORMAT_HIZ:
            return zip_list(arc, entries, count);
        case L1Z_FORMAT_1Z:
        case L1Z_FORMAT_1ZIP:
            if (!arc->entries) {
                fflush(arc->fp);
                int ret = f1z_read_index(arc);
                if (ret != L1Z_OK) { *entries = NULL; *count = 0; return L1Z_OK; }
            }
            *entries = arc->entries;
            *count = arc->entry_count;
            return L1Z_OK;
        case L1Z_FORMAT_AF:
            if (!arc->entries) {
                fflush(arc->fp);
                int ret = af_read_index(arc);
                if (ret != L1Z_OK) { *entries = NULL; *count = 0; return L1Z_OK; }
            }
            *entries = arc->entries;
            *count = arc->entry_count;
            return L1Z_OK;
        case L1Z_FORMAT_7Z:
        case L1Z_FORMAT_RAR:
        case L1Z_FORMAT_ISO:
        case L1Z_FORMAT_IMG:
            *entries = NULL;
            *count = 0;
            return L1Z_OK;
        default:
            return L1Z_E_UNSUPPORTED;
    }
}

void l1z_free_entries(L1Z_Entry* entries, int count) {
    (void)count;
    if (entries) free(entries);
}

int l1z_test(L1Z_Archive* arc, const char* password) {
    if (!arc) return L1Z_E_PARAM;

    int ret = L1Z_OK;
    if (arc->format == L1Z_FORMAT_ZIP || arc->format == L1Z_FORMAT_EZ || arc->format == L1Z_FORMAT_HIZ) {
        if (!arc->entries) {
            ret = zip_read_central_dir(arc);
            if (ret != L1Z_OK) return ret;
        }
        for (int i = 0; i < arc->entry_count; i++) {
            report_progress(arc->entries[i].filename, (i * 100) / (arc->entry_count > 0 ? arc->entry_count : 1));
            ret = zip_test_entry(arc, i, password);
            if (ret != L1Z_OK) break;
        }
    }
    /* For custom formats, test would involve reading CRC, etc. */
    return ret;
}

int l1z_delete(L1Z_Archive* arc, const char* pattern) {
    (void)pattern;
    if (!arc) return L1Z_E_PARAM;
    /* Delete is complex for most formats as it requires rewriting the archive.
       For now, we note this as a limitation. */
    return L1Z_E_UNSUPPORTED;
}

int l1z_update(L1Z_Archive* arc, const char* filepath,
               const char* password, int level, int recursive,
               const char* exclude)
{
    (void)password;
    /* Update = delete + add */
    int ret = l1z_delete(arc, filepath);
    if (ret != L1Z_OK && ret != L1Z_E_UNSUPPORTED) return ret;
    return l1z_add(arc, filepath, password, level, recursive, exclude);
}
