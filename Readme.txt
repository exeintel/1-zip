================================================================================
  1-zip v1.0.0 - Console Archiver
  Developer: ExEintel
================================================================================

DESCRIPTION
===========
1-zip is a console-mode archiver utility inspired by 7-Zip. It provides
command-line archive management with support for multiple formats including
ZIP, its own 1Z/1ZIP format, AF (Application File), EZ, HIZ, and partial
support for 7Z, RAR, ISO, and IMG formats.

PROJECT STRUCTURE
=================
1-zip\
  |-- 1zip.exe          - Command-line shell (CLI frontend)
  |-- lib1zip.dll        - Core library with all archiving logic
  |-- Readme.txt          - This file
  |-- sourse\
  |     |-- main.c        - Source for 1zip.exe
  |     |-- lib1zip.c     - Source for lib1zip.dll
  |     |-- lib1zip.h     - Public API header for lib1zip.dll


SYNTAX
======
  1-zip <command> [options] <archive> [files...]


COMMANDS
========
  Command  Description
  -------  --------------------------------------------
  a        Add files to archive (create / append)
  x        Extract archive with full directory paths
  e        Extract archive without paths (to current folder)
  l        List contents of archive
  t        Test integrity of archive
  d        Delete files from archive
  u        Update archive (add only changed files)


OPTIONS
=======
  Option         Description
  -------        --------------------------------------------
  -p<password>   Set password protection (e.g. -p12345)
  -m<level>      Compression level:
                   0  = store (no compression)
                   1-3 = fast compression
                   4-6 = normal compression
                   7-9 = maximum compression
  -e<masks>      Exclude files/masks (semicolon-separated)
  -r             Process subdirectories recursively
  -o<dir>        Output directory for extraction
  -oy            Overwrite existing files (yes)
  -on            Skip existing files (no)
  -oa            Overwrite all without asking (all)
  -v<size>       Create multi-volume archive
                 Examples: -v10M, -v100K, -v1G
  -s             Solid archive (improves compression ratio)
  -q             Quiet mode (suppress progress bar)
  -c<encoding>   Filename encoding (UTF-8, CP866)


SUPPORTED FORMATS
=================
  Format   Extension   Description
  ------   ---------   --------------------------------------------
  ZIP      .zip        Standard ZIP archive (read/write via zlib deflate)
  1Z       .1z         Native 1-zip compressed archive (own format)
  1ZIP     .1zip       Alias for .1z format
  AF       .af         Application File - no compression, no password
  EZ       .Ez         ExEintel ZIP (ZIP with different extension)
  HIZ      .hiz        Hi ZIP (ZIP with different extension)
  7Z       .7z          7-Zip format (read-only, limited)
  RAR      .rar        WinRAR format (read-only, limited)
  ISO      .iso        Disc image (read-only, limited)
  IMG      .img        Disc image (read-only, limited)


OWN FORMAT SPECIFICATIONS
=========================

1Z / 1ZIP Format
----------------
  Header (8 bytes):
    Bytes 0-1:  Magic "1Z" (0x31 0x5A)
    Byte 2:     Version (1)
    Byte 3-7:   Reserved (zero)

  After header:
    File count:    4 bytes (unsigned 32-bit LE)
    For each file:
      Filename len:    2 bytes (unsigned 16-bit LE)
      Filename:        variable (UTF-8)
      Uncomp. size:    8 bytes (unsigned 64-bit LE)
      Comp. size:      8 bytes (unsigned 64-bit LE)
      CRC32:           4 bytes (unsigned 32-bit LE)
      Flags:           1 byte (bit 0=is_dir, bit 1=compressed)
      Compressed data: variable

  Compression: Deflate (zlib) when flag bit 1 is set.

AF Format (Application File)
----------------------------
  Header (4 bytes):
    Bytes 0-1:  Magic "AF" (0x41 0x46)
    Byte 2:     Version (1)
    Byte 3:     Reserved (zero)

  After header:
    File count:    4 bytes (unsigned 32-bit LE)
    For each file:
      Filename len:    2 bytes (unsigned 16-bit LE)
      Filename:        variable (UTF-8)
      File size:       8 bytes (unsigned 64-bit LE)
      Raw file data:   variable (no compression)


EXAMPLES
========

  1. Create archive with maximum compression (recursive):
     1-zip a -r -m9 backup.zip C:\Users\Documents

  2. Create archive with no compression:
     1-zip a -m0 quick.zip file.txt

  3. Extract archive preserving full paths:
     1-zip x archive.1z -o C:\extracted

  4. Extract archive without paths (all files in current dir):
     1-zip e archive.zip

  5. Extract with password:
     1-zip x -pMySecret secret.1z -o C:\out

  6. List archive contents:
     1-zip l data.zip

  7. Test archive integrity:
     1-zip t backup.1z

  8. Create AF (no-compression) archive:
     1-zip a files.af file1.txt file2.jpg

  9. Create multi-volume archive (10 MB each):
     1-zip a -v10M large.zip hugefile.iso

  10. Quick extract (no overwrite):
      1-zip x -on readme.zip


EXIT CODES
==========
  0     Success
  1     General error


BUILD REQUIREMENTS
==================
  - MinGW GCC (GCC for Windows)
  - zlib development library (libz.a, zlib.h)
  - Build command: build.bat


LIBRARY API (lib1zip.dll)
=========================
  The DLL exports the following functions:

  L1Z_Archive* l1z_open(const char* path, const char* mode, int format)
  int l1z_close(L1Z_Archive* arc)
  int l1z_add(L1Z_Archive* arc, const char* filepath, const char* password,
              int level, int recursive, const char* exclude)
  int l1z_extract(L1Z_Archive* arc, const char* output_dir,
                  int preserve_paths, const char* password, int overwrite)
  int l1z_list(L1Z_Archive* arc, L1Z_Entry** entries, int* count)
  int l1z_test(L1Z_Archive* arc, const char* password)
  int l1z_delete(L1Z_Archive* arc, const char* pattern)
  int l1z_update(L1Z_Archive* arc, const char* filepath,
                 const char* password, int level, int recursive,
                 const char* exclude)
  const char* l1z_strerror(int code)
  int l1z_detect_format(const char* path)
  void l1z_set_progress_callback(void (*cb)(const char*, int))

  Error codes:
    L1Z_OK             0    Success
    L1Z_E_OPEN        -1    Cannot open archive
    L1Z_E_READ        -2    Read error
    L1Z_E_WRITE       -3    Write error
    L1Z_E_MEMORY      -4    Out of memory
    L1Z_E_FORMAT      -5    Unsupported or corrupted format
    L1Z_E_COMPRESS    -6    Compression error
    L1Z_E_DECOMPRESS  -7    Decompression error
    L1Z_E_NOTFOUND    -8    File not found in archive
    L1Z_E_EXISTS      -9    File already exists
    L1Z_E_PASSWORD   -10    Wrong password or not encrypted
    L1Z_E_UNSUPPORTED -11   Format not yet supported
    L1Z_E_PARAM      -12    Invalid parameter
    L1Z_E_CREATE     -13    Cannot create output file
    L1Z_E_CRC        -14    CRC check failed


LICENSE
=======
This software is developed by ExEintel.
All rights reserved.
