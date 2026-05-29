#include "lib1zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define VERSION "1.0.0"
#define AUTHOR  "ExEintel"

static void print_banner(void) {
    printf("1-zip v%s - Console archiver by %s\n", VERSION, AUTHOR);
    printf("========================================\n\n");
}

static void print_usage(void) {
    print_banner();
    printf("Usage:\n");
    printf("  1-zip <command> [options] <archive> [files...]\n\n");
    printf("Commands:\n");
    printf("  a   Add files to archive (create/append)\n");
    printf("  x   Extract archive with full paths\n");
    printf("  e   Extract archive without paths (to current folder)\n");
    printf("  l   List archive contents\n");
    printf("  t   Test archive integrity\n");
    printf("  d   Delete files from archive\n");
    printf("  u   Update archive (only changed files)\n\n");
    printf("Options:\n");
    printf("  -p<password>    Set password\n");
    printf("  -m<level>       Compression level: 0=store, 1-3=fast, 4-6=normal, 7-9=max\n");
    printf("  -e<mask>        Exclude files/masks (use ; to separate multiple)\n");
    printf("  -r              Process subfolders recursively\n");
    printf("  -o{y|n|a}       Overwrite mode: y=yes, n=no, a=all\n");
    printf("  -v<size>        Volume size (e.g. 10M, 100K)\n");
    printf("  -s              Solid archive (better compression)\n");
    printf("  -q              Quiet mode (no progress bar)\n");
    printf("  -c<encoding>    Filename encoding (UTF-8, CP866)\n\n");
    printf("Supported formats:\n");
    printf("  ZIP, 1Z, 1ZIP, AF, EZ, HIZ, 7Z, RAR, ISO, IMG\n\n");
    printf("Examples:\n");
    printf("  1-zip a -r -m9 backup.zip C:\\docs\n");
    printf("  1-zip x -p123 secret.7z -o C:\\out\n");
    printf("  1-zip l data.7z\n");
    printf("  1-zip u -r project.zip src\\\n");
    printf("  1-zip a -v10M large.zip hugefile.iso\n");
}

static int parse_overwrite(const char* s) {
    while (*s && *s != '-') s++;
    if (*s == 'o' || *s == 'O') {
        s++;
        if (*s == 'y' || *s == 'Y') return L1Z_OVERWRITE_YES;
        if (*s == 'n' || *s == 'N') return L1Z_OVERWRITE_NO;
        if (*s == 'a' || *s == 'A') return L1Z_OVERWRITE_ALL;
    }
    return L1Z_OVERWRITE_YES;
}

static int parse_volume(const char* s, unsigned long long* size) {
    while (*s && *s != 'v') s++;
    if (*s == 'v' || *s == 'V') {
        s++;
        char* end;
        double val = strtod(s, &end);
        if (end == s) return -1;
        if (*end == 'K' || *end == 'k') *size = (unsigned long long)(val * 1024);
        else if (*end == 'M' || *end == 'm') *size = (unsigned long long)(val * 1024 * 1024);
        else if (*end == 'G' || *end == 'g') *size = (unsigned long long)(val * 1024 * 1024 * 1024);
        else *size = (unsigned long long)val;
        return 0;
    }
    return -1;
}

static int parse_password(const char* s, char* pwd, int maxlen) {
    while (*s && *s != 'p' && *s != 'P') s++;
    if (*s == 'p' || *s == 'P') {
        s++;
        int i;
        for (i = 0; s[i] && i < maxlen - 1; i++)
            pwd[i] = s[i];
        pwd[i] = '\0';
        return (int)i;
    }
    pwd[0] = '\0';
    return 0;
}

static int parse_level(const char* s) {
    while (*s && *s != 'm' && *s != 'M') s++;
    if (*s == 'm' || *s == 'M') {
        s++;
        if (*s >= '0' && *s <= '9')
            return *s - '0';
    }
    return 6;
}

static int has_recursive(const char** args, int count) {
    for (int i = 0; i < count; i++) {
        if (args[i][0] == '-' && (args[i][1] == 'r' || args[i][1] == 'R'))
            return 1;
    }
    return 0;
}

static int is_option(const char* s) {
    return s[0] == '-';
}

static int detect_format_from_ext(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return L1Z_FORMAT_ZIP;
    ext++;
    char low[32];
    int i;
    for (i = 0; ext[i] && i < 31; i++)
        low[i] = (ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i];
    low[i] = '\0';

    if (strcmp(low, "zip") == 0) return L1Z_FORMAT_ZIP;
    if (strcmp(low, "1z") == 0) return L1Z_FORMAT_1Z;
    if (strcmp(low, "1zip") == 0) return L1Z_FORMAT_1ZIP;
    if (strcmp(low, "af") == 0) return L1Z_FORMAT_AF;
    if (strcmp(low, "ez") == 0) return L1Z_FORMAT_EZ;
    if (strcmp(low, "hiz") == 0) return L1Z_FORMAT_HIZ;
    return L1Z_FORMAT_ZIP;
}

static int format_to_int(const char* name) {
    char low[32];
    int i;
    for (i = 0; name[i] && i < 31; i++)
        low[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    low[i] = '\0';
    if (strcmp(low, "zip") == 0) return L1Z_FORMAT_ZIP;
    if (strcmp(low, "1z") == 0) return L1Z_FORMAT_1Z;
    if (strcmp(low, "1zip") == 0) return L1Z_FORMAT_1ZIP;
    if (strcmp(low, "af") == 0) return L1Z_FORMAT_AF;
    if (strcmp(low, "ez") == 0) return L1Z_FORMAT_EZ;
    if (strcmp(low, "hiz") == 0) return L1Z_FORMAT_HIZ;
    return L1Z_FORMAT_ZIP;
}

static const char* format_str(int fmt) {
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

static const char* size_str(unsigned long long size) {
    static char buf[64];
    if (size < 1024)
        snprintf(buf, sizeof(buf), "%llu B", size);
    else if (size < 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", (double)size / 1024);
    else if (size < 1024ULL * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", (double)size / (1024 * 1024));
    else
        snprintf(buf, sizeof(buf), "%.2f GB", (double)size / (1024 * 1024 * 1024));
    return buf;
}

static void progress_callback(const char* file, int percent) {
    static int last_percent = -1;
    if (percent == last_percent) return;
    last_percent = percent;
    printf("\r[%3d%%] %s    ", percent, file ? file : "");
    fflush(stdout);
    if (percent >= 100) printf("\n");
}

static int cmd_add(const char* archive, const char** files, int file_count,
                   const char** args, int arg_count)
{
    char password[256] = "";
    int level = 6;
    int recursive = has_recursive(args, arg_count);

    for (int i = 0; i < arg_count; i++) {
        if (args && args[i][0] == '-') {
            switch (args[i][1]) {
                case 'p': case 'P': parse_password(args[i], password, sizeof(password)); break;
                case 'm': case 'M': level = parse_level(args[i]); break;
            }
        }
    }

    int fmt = detect_format_from_ext(archive);
    L1Z_Archive* arc = l1z_open(archive, "r+b", fmt);
    if (!arc) {
        arc = l1z_open(archive, "wb", fmt);
    }
    if (!arc) {
        fprintf(stderr, "Error: Cannot open archive '%s'\n", archive);
        return 1;
    }

    l1z_set_progress_callback(progress_callback);

    int ret = L1Z_OK;
    for (int i = 0; i < file_count; i++) {
        ret = l1z_add(arc, files[i], password[0] ? password : NULL,
                      level, recursive, NULL);
        if (ret != L1Z_OK) {
            fprintf(stderr, "Error adding '%s': %s\n", files[i], l1z_strerror(ret));
            break;
        }
    }

    if (ret == L1Z_OK)
        printf("Successfully added %d file(s) to '%s'\n", file_count, archive);

    l1z_close(arc);
    return ret == L1Z_OK ? 0 : 1;
}

static int cmd_extract(const char* archive, const char* output_dir,
                       int preserve_paths, const char** args, int arg_count)
{
    char password[256] = "";
    int overwrite = L1Z_OVERWRITE_YES;

    for (int i = 0; i < arg_count; i++) {
        if (args && args[i][0] == '-') {
            switch (args[i][1]) {
                case 'p': case 'P': parse_password(args[i], password, sizeof(password)); break;
                case 'o': case 'O': overwrite = parse_overwrite(args[i]); break;
            }
        }
    }

    int fmt = l1z_detect_format(archive);
    if (fmt == L1Z_FORMAT_UNKNOWN) fmt = detect_format_from_ext(archive);

    L1Z_Archive* arc = l1z_open(archive, "rb", fmt);
    if (!arc) {
        fprintf(stderr, "Error: Cannot open archive '%s'\n", archive);
        return 1;
    }

    /* If output_dir is not set, use current dir or extract based on -o option */
    /* We need to find the -o path, which is different from overwrite -oy/-on/-oa */
    const char* actual_outdir = output_dir ? output_dir : ".";

    l1z_set_progress_callback(progress_callback);
    int ret = l1z_extract(arc, actual_outdir, preserve_paths,
                          password[0] ? password : NULL, overwrite);

    if (ret == L1Z_OK)
        printf("Successfully extracted to '%s'\n", actual_outdir);
    else
        fprintf(stderr, "Extraction error: %s\n", l1z_strerror(ret));

    l1z_close(arc);
    return ret == L1Z_OK ? 0 : 1;
}

static int cmd_list(const char* archive) {
    int fmt = l1z_detect_format(archive);
    if (fmt == L1Z_FORMAT_UNKNOWN) fmt = detect_format_from_ext(archive);

    L1Z_Archive* arc = l1z_open(archive, "rb", fmt);
    if (!arc) {
        fprintf(stderr, "Error: Cannot open archive '%s'\n", archive);
        return 1;
    }

    L1Z_Entry* entries = NULL;
    int count = 0;
    int ret = l1z_list(arc, &entries, &count);
    if (ret != L1Z_OK) {
        fprintf(stderr, "Error listing archive: %s\n", l1z_strerror(ret));
        l1z_close(arc);
        return 1;
    }

    printf("Archive: %s\n", archive);
    printf("Format: %s\n", format_str(fmt));
    printf("Contents: %d file(s)\n\n", count);
    printf("%-4s %-50s %-12s %-12s %-10s\n", "#", "Name", "Size", "Compressed", "CRC32");
    printf("%-4s %-50s %-12s %-12s %-10s\n", "----", "--------------------------------------------------",
           "------------", "------------", "----------");

    unsigned long long total_uncomp = 0, total_comp = 0;
    for (int i = 0; i < count; i++) {
        printf("%-4d %-50s %-12s %-12s %08X\n",
               i + 1,
               entries[i].filename,
               size_str(entries[i].uncompressed_size),
               size_str(entries[i].compressed_size),
               entries[i].crc32);
        total_uncomp += entries[i].uncompressed_size;
        total_comp += entries[i].compressed_size;
    }

    printf("%-4s %-50s %-12s %-12s %-10s\n", "----", "--------------------------------------------------",
           "------------", "------------", "----------");
    printf("%-4s %-50s %-12s %-12s\n", "Total", "", size_str(total_uncomp), size_str(total_comp));

    /* entries are owned by archive, freed by l1z_close */
    l1z_close(arc);
    return 0;
}

static int cmd_test(const char* archive) {
    int fmt = l1z_detect_format(archive);
    if (fmt == L1Z_FORMAT_UNKNOWN) fmt = detect_format_from_ext(archive);

    L1Z_Archive* arc = l1z_open(archive, "rb", fmt);
    if (!arc) {
        fprintf(stderr, "Error: Cannot open archive '%s'\n", archive);
        return 1;
    }

    printf("Testing archive: %s\n", archive);
    l1z_set_progress_callback(progress_callback);

    int ret = l1z_test(arc, NULL);
    if (ret == L1Z_OK)
        printf("\nArchive integrity check passed.\n");
    else
        fprintf(stderr, "\nIntegrity check failed: %s\n", l1z_strerror(ret));

    l1z_close(arc);
    return ret == L1Z_OK ? 0 : 1;
}

static int cmd_delete(const char* archive, const char* pattern) {
    int fmt = detect_format_from_ext(archive);

    L1Z_Archive* arc = l1z_open(archive, "r+b", fmt);
    if (!arc) {
        fprintf(stderr, "Error: Cannot open archive '%s'\n", archive);
        return 1;
    }

    int ret = l1z_delete(arc, pattern);
    if (ret == L1Z_OK)
        printf("Deleted matching files from '%s'\n", archive);
    else
        fprintf(stderr, "Delete error: %s\n", l1z_strerror(ret));

    l1z_close(arc);
    return ret == L1Z_OK ? 0 : 1;
}

static int cmd_update(const char* archive, const char** files, int file_count,
                      const char** args, int arg_count)
{
    char password[256] = "";
    int level = 6;
    int recursive = has_recursive(args, arg_count);

    for (int i = 0; i < arg_count; i++) {
        if (args && args[i][0] == '-') {
            switch (args[i][1]) {
                case 'p': case 'P': parse_password(args[i], password, sizeof(password)); break;
                case 'm': case 'M': level = parse_level(args[i]); break;
            }
        }
    }

    int fmt = detect_format_from_ext(archive);
    L1Z_Archive* arc = l1z_open(archive, "r+b", fmt);
    if (!arc) {
        arc = l1z_open(archive, "wb", fmt);
    }
    if (!arc) {
        fprintf(stderr, "Error: Cannot open archive '%s'\n", archive);
        return 1;
    }

    l1z_set_progress_callback(progress_callback);

    int ret = L1Z_OK;
    for (int i = 0; i < file_count; i++) {
        ret = l1z_update(arc, files[i], password[0] ? password : NULL,
                         level, recursive, NULL);
        if (ret != L1Z_OK) {
            fprintf(stderr, "Error updating '%s': %s\n", files[i], l1z_strerror(ret));
            break;
        }
    }

    if (ret == L1Z_OK)
        printf("Successfully updated '%s'\n", archive);

    l1z_close(arc);
    return ret == L1Z_OK ? 0 : 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char* cmd = argv[1];

    /* Handle help */
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "/?") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    /* Handle version */
    if (strcmp(cmd, "--version") == 0) {
        printf("1-zip v%s by %s\n", VERSION, AUTHOR);
        return 0;
    }

    /* Parse: command [options] archive [files...] */
    const char* archive = NULL;
    const char** options = NULL;
    const char** files = NULL;
    int option_count = 0;
    int file_count = 0;
    int archive_found = 0;

    /* Simple argument parsing: collect options (-xxx), find archive (first non-option),
       and files (remaining non-option args) */
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-') {
            option_count++;
        } else if (!archive_found) {
            archive = argv[i];
            archive_found = 1;
        } else {
            file_count++;
        }
    }

    if (!archive && cmd[0] != 'l') {
        fprintf(stderr, "Error: No archive specified.\n");
        print_usage();
        return 1;
    }

    /* Build options array */
    if (option_count > 0) {
        options = (const char**)malloc((size_t)option_count * sizeof(char*));
        int idx = 0;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '-') {
                options[idx++] = argv[i];
            }
        }
    }

    /* Build files array */
    if (file_count > 0) {
        files = (const char**)malloc((size_t)file_count * sizeof(char*));
        int idx = 0;
        int archive_passed = 0;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] != '-') {
                if (!archive_passed) {
                    archive_passed = 1;
                } else {
                    files[idx++] = argv[i];
                }
            }
        }
    }

    /* Handle -o (output directory) separately from overwrite -oy/-on/-oa */
    const char* output_dir = NULL;
    for (int i = 0; i < option_count; i++) {
        if (options[i][0] == '-' && options[i][1] == 'o') {
            const char* val = options[i] + 2;
            if (val[0] && val[0] != 'y' && val[0] != 'n' && val[0] != 'Y' && val[0] != 'N') {
                output_dir = val;
            }
        }
    }

    int result = 0;

    if (strcmp(cmd, "a") == 0) {
        result = cmd_add(archive, files, file_count, options, option_count);
    } else if (strcmp(cmd, "x") == 0) {
        /* x: extract with paths */
        result = cmd_extract(archive, output_dir ? output_dir : ".", 1, options, option_count);
    } else if (strcmp(cmd, "e") == 0) {
        /* e: extract without paths */
        result = cmd_extract(archive, output_dir ? output_dir : ".", 0, options, option_count);
    } else if (strcmp(cmd, "l") == 0) {
        /* l: list - archive is the first non-option after command */
        const char* list_archive = NULL;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] != '-') { list_archive = argv[i]; break; }
        }
        if (!list_archive) {
            fprintf(stderr, "Error: No archive specified for listing.\n");
            return 1;
        }
        result = cmd_list(list_archive);
    } else if (strcmp(cmd, "t") == 0) {
        const char* test_archive = NULL;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] != '-') { test_archive = argv[i]; break; }
        }
        if (!test_archive) {
            fprintf(stderr, "Error: No archive specified for testing.\n");
            return 1;
        }
        result = cmd_test(test_archive);
    } else if (strcmp(cmd, "d") == 0) {
        const char* pattern = file_count > 0 ? files[0] : NULL;
        result = cmd_delete(archive, pattern);
    } else if (strcmp(cmd, "u") == 0) {
        result = cmd_update(archive, files, file_count, options, option_count);
    } else {
        fprintf(stderr, "Error: Unknown command '%s'\n", cmd);
        print_usage();
        result = 1;
    }

    free((void*)options);
    free((void*)files);
    return result;
}
