#ifndef LIB1ZIP_H
#define LIB1ZIP_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef LIB1ZIP_EXPORTS
    #define L1Z_API __declspec(dllexport)
  #else
    #define L1Z_API __declspec(dllimport)
  #endif
#else
  #define L1Z_API
#endif

#define L1Z_FORMAT_UNKNOWN   0
#define L1Z_FORMAT_ZIP       1
#define L1Z_FORMAT_1Z        2
#define L1Z_FORMAT_1ZIP      3
#define L1Z_FORMAT_AF        4
#define L1Z_FORMAT_EZ        5
#define L1Z_FORMAT_HIZ       6
#define L1Z_FORMAT_7Z        7
#define L1Z_FORMAT_RAR       8
#define L1Z_FORMAT_ISO       9
#define L1Z_FORMAT_IMG      10

#define L1Z_OK               0
#define L1Z_E_OPEN          -1
#define L1Z_E_READ          -2
#define L1Z_E_WRITE         -3
#define L1Z_E_MEMORY        -4
#define L1Z_E_FORMAT        -5
#define L1Z_E_COMPRESS      -6
#define L1Z_E_DECOMPRESS    -7
#define L1Z_E_NOTFOUND      -8
#define L1Z_E_EXISTS        -9
#define L1Z_E_PASSWORD      -10
#define L1Z_E_UNSUPPORTED   -11
#define L1Z_E_PARAM         -12
#define L1Z_E_CREATE        -13
#define L1Z_E_CRC           -14

#define L1Z_OVERWRITE_NO     0
#define L1Z_OVERWRITE_YES    1
#define L1Z_OVERWRITE_ALL    2

#define L1Z_COMPRESS_STORE   0
#define L1Z_COMPRESS_FAST    3
#define L1Z_COMPRESS_NORMAL  6
#define L1Z_COMPRESS_MAX     9

#define L1Z_MODE_READ   "rb"
#define L1Z_MODE_WRITE  "wb"
#define L1Z_MODE_UPDATE "r+b"

typedef struct L1Z_Entry {
    char  filename[1024];
    unsigned long long uncompressed_size;
    unsigned long long compressed_size;
    unsigned long crc32;
    int    is_dir;
    int    index;
    unsigned int local_offset;
} L1Z_Entry;

typedef struct L1Z_Archive L1Z_Archive;

L1Z_API L1Z_Archive* l1z_open(const char* path, const char* mode, int format);
L1Z_API int l1z_close(L1Z_Archive* arc);
L1Z_API int l1z_add(L1Z_Archive* arc, const char* filepath, const char* password,
                     int level, int recursive, const char* exclude);
L1Z_API int l1z_extract(L1Z_Archive* arc, const char* output_dir,
                         int preserve_paths, const char* password, int overwrite);
L1Z_API int l1z_list(L1Z_Archive* arc, L1Z_Entry** entries, int* count);
L1Z_API void l1z_free_entries(L1Z_Entry* entries, int count);
L1Z_API int l1z_test(L1Z_Archive* arc, const char* password);
L1Z_API int l1z_delete(L1Z_Archive* arc, const char* pattern);
L1Z_API int l1z_update(L1Z_Archive* arc, const char* filepath,
                        const char* password, int level, int recursive,
                        const char* exclude);
L1Z_API const char* l1z_strerror(int code);
L1Z_API int l1z_detect_format(const char* path);
L1Z_API void l1z_set_progress_callback(void (*cb)(const char* file, int percent));

#ifdef __cplusplus
}
#endif

#endif
