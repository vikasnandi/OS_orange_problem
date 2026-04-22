// tree.c — Tree object serialization and construction
//
// Binary tree format (per entry, concatenated, no separators):
//   "<mode-octal-ascii> <name>\0<32-byte-binary-hash>"
//
// PROVIDED: get_file_mode, tree_parse, tree_serialize
// IMPLEMENTED: tree_from_index

#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>

// object_write is defined in object.c; pes.h doesn't declare it
int object_write(ObjectType type, const void *data, size_t size, ObjectID *id_out);
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ─── Mode constants ──────────────────────────────────────────────────────────

#define MODE_FILE  0100644
#define MODE_EXEC  0100755
#define MODE_DIR   0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))   return MODE_DIR;
    if (st.st_mode & S_IXUSR)  return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *e = &tree_out->entries[tree_out->count];

        // 1. Find the space separating mode from name
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        e->mode = (uint32_t)strtol(mode_str, NULL, 8);
        ptr = space + 1;

        // 2. Find the NUL terminating the name
        const uint8_t *nul = memchr(ptr, '\0', end - ptr);
        if (!nul) return -1;

        size_t name_len = nul - ptr;
        if (name_len >= sizeof(e->name)) return -1;
        memcpy(e->name, ptr, name_len);
        e->name[name_len] = '\0';
        ptr = nul + 1;

        // 3. Read 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(e->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Comparator: sort entries by name for deterministic hashing
static int cmp_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name,
                  ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into the binary wire format.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // max per entry: 7 (mode) + 1 (space) + 255 (name) + 1 (NUL) + 32 (hash)
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buf = malloc(max_size);
    if (!buf) return -1;

    // Sort a copy so the output is always deterministic
    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), cmp_entries);

    size_t off = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];
        // Write "<mode-octal> <name>\0"
        int n = sprintf((char *)buf + off, "%o %s", e->mode, e->name);
        off += n + 1;           // +1 includes the '\0' sprintf wrote
        // Write raw 32-byte hash
        memcpy(buf + off, e->hash.hash, HASH_SIZE);
        off += HASH_SIZE;
    }

    *data_out = buf;
    *len_out  = off;
    return 0;
}

// ─── tree_from_index (recursive) ────────────────────────────────────────────

// Builds one level of the tree from a sorted slice of index entries.
//
//   entries  : pointer into the index entries array for this slice
//   count    : number of entries in this slice
//   prefix   : directory prefix for this level, e.g. "src/" (root = "")
//   id_out   : receives the ObjectID of the written tree object
static int write_tree_level(IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *rel   = entries[i].path + strlen(prefix);
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // ── Direct file at this level ──
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = entries[i].hash;
            i++;
        } else {
            // ── Subdirectory: group all entries sharing this dir name ──
            size_t dir_len = (size_t)(slash - rel);

            char dir_name[256] = {0};
            strncpy(dir_name, rel, dir_len);

            // Full sub-prefix, e.g. "src/" or "a/b/"
            char sub_prefix[600] = {0};
            snprintf(sub_prefix, sizeof(sub_prefix),
                     "%s%s/", prefix, dir_name);

            // Count consecutive entries belonging to this subdir
            int j = i;
            while (j < count &&
                   strncmp(entries[j].path, sub_prefix,
                            strlen(sub_prefix)) == 0)
                j++;

            // Recurse: build subtree for entries[i..j)
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i,
                                 sub_prefix, &sub_id) != 0)
                return -1;

            // Add a single DIR entry pointing at the subtree
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = sub_id;

            i = j;
        }
    }

    // Serialize and store this tree object
    void  *blob     = NULL;
    size_t blob_len = 0;
    if (tree_serialize(&tree, &blob, &blob_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, blob, blob_len, id_out);
    free(blob);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree objects
// to the object store.  Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index idx;
    if (index_load(&idx) != 0) return -1;
    if (idx.count == 0)        return -1;   // nothing staged

    return write_tree_level(idx.entries, idx.count, "", id_out);
}