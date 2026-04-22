// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// object_write is defined in object.c; pes.h doesn't declare it
int object_write(ObjectType type, const void *data, size_t size, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

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

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                   st.st_size  != (off_t)index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path);
            unstaged_count++;
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")   != NULL) continue;

            int tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    tracked = 1; break;
                }
            }
            if (!tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── Helper: qsort comparator ────────────────────────────────────────────────

static int cmp_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index.
// A missing file is treated as an empty index (not an error).
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        if (errno == ENOENT) return 0;   // no index yet — that's fine
        return -1;
    }

    char     hex[HASH_HEX_SIZE + 1];
    unsigned int  mode;
    unsigned long mtime_sec, size;
    char     path[512];

    // Each line: <mode-octal> <hex-hash> <mtime> <size> <path>
    while (index->count < MAX_INDEX_ENTRIES &&
           fscanf(f, "%o %64s %lu %lu %511s",
                  &mode, hex, &mtime_sec, &size, path) == 5) {

        IndexEntry *e = &index->entries[index->count];
        e->mode      = mode;
        e->mtime_sec = (uint64_t)mtime_sec;
        e->size      = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically (temp-file + rename).
// Entries are sorted by path before writing.
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Sort a copy so the on-disk file is always in path order
    Index sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry),
          cmp_index_entries);

    // Write to a temp file in the same directory
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp_XXXXXX", INDEX_FILE);

    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_path); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %lu %lu %s\n",
                e->mode, hex,
                (unsigned long)e->mtime_sec,
                (unsigned long)(uint32_t)e->size,
                e->path);
    }

    // Flush → fsync → atomic rename
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f); unlink(tmp_path); return -1;
    }
    fclose(f);

    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path); return -1;
    }
    return 0;
}

// Stage a file: hash it as a blob, store it, update the index entry.
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // 1. Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long signed_size = ftell(f);
    rewind(f);
    if (signed_size < 0) { fclose(f); return -1; }

    size_t file_size = (size_t)signed_size;
    void  *contents  = malloc(file_size ? file_size : 1);  // avoid malloc(0)
    if (!contents) { fclose(f); return -1; }

    if (file_size > 0 && fread(contents, 1, file_size, f) != file_size) {
        free(contents); fclose(f); return -1;
    }
    fclose(f);

    // 2. Write blob to object store
    ObjectID id;
    if (object_write(OBJ_BLOB, contents, file_size, &id) != 0) {
        free(contents); return -1;
    }
    free(contents);

    // 3. Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    // 4. Update existing entry or append a new one
    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index full\n");
            return -1;
        }
        e = &index->entries[index->count++];
    }

    e->hash      = id;
    e->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint32_t)st.st_size;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';

    // 5. Persist atomically
    return index_save(index);
}