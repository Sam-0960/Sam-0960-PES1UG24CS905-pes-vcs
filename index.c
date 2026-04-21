// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
    }

    printf("\nUnstaged changes:\n");
    // keep empty for Phase 3 (avoid wrong "modified")

    printf("\nUntracked files:\n");

    DIR *d = opendir(".");
    if (!d) return -1;

    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        int found = 0;
        for (int i = 0; i < index->count; i++) {
            if (strcmp(index->entries[i].path, entry->d_name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            printf("  untracked:  %s\n", entry->d_name);
        }
    }

    closedir(d);
    return 0;
} 
//DO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *idx) {
    FILE *f = fopen(INDEX_FILE, "r");

    if (!f) {
        idx->count = 0;
        return 0;
    }

    idx->count = 0;

    char hex[HASH_HEX_SIZE + 1];

    while (idx->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &idx->entries[idx->count];

        if (fscanf(f, "%o %64s %255s\n",
                   &e->mode,
                   hex,
                   e->path) != 3) {
            break;
        }

        // convert hex → binary
        hex_to_hash(hex, &e->hash);

        idx->count++;
    }

   fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // temp file (atomic write)
    const char *tmp = ".pes/index.tmp";

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &index->entries[i];

        // convert hash → hex
        hash_to_hex(&e->hash, hex);

        fprintf(f, "%o %s %s\n",
                e->mode,
                hex,
                e->path);
    }

    fclose(f);

    // atomic rename
    if (rename(tmp, INDEX_FILE) != 0) {
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // Read file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void *buf = malloc(size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    fread(buf, 1, size, f);
    fclose(f);

    // Write object (blob)
    ObjectID id;
    if (object_write(OBJ_BLOB, buf, size, &id) < 0) {
        free(buf);
        return -1;
    }

    free(buf);

    //  CHECK FOR EXISTING ENTRY
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            // Update existing entry
            index->entries[i].hash = id;
            return 0;
        }
    }

    // Add new entry
    IndexEntry *e = &index->entries[index->count++];

    e->mode = 0100644;
    strcpy(e->path, path);
    e->hash = id;

    return 0;
}
