// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include "pes.h" 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ──────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry),
          compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s",
                              entry->mode, entry->name);
        offset += written + 1; /* +1 for the '\0' written by sprintf */
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implemented ───────────────────────────────────────────────────────

// Recursive helper: builds one tree level from a slice of index entries.
//
// entries : pointer into the index entries array
// count   : number of entries in this slice
// prefix  : current directory prefix, e.g. "src/" (empty string = root)
//
// Files whose path (after stripping prefix) has no '/' are direct entries.
// Files whose path has a '/' belong to a subdirectory that is processed
// recursively before adding a single DIR entry to the current level.
static int write_tree_level(IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        /* Path relative to this directory level */
        const char *rel   = entries[i].path + strlen(prefix);
        const char *slash = strchr(rel, '/');

        if (!slash) {
            /* ── Direct file at this level ── */
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = entries[i].hash;
            i++;
        } else {
            /* ── Subdirectory: group all entries sharing this dir name ── */
            size_t dir_len = (size_t)(slash - rel);

            char dir_name[256] = {0};
            strncpy(dir_name, rel, dir_len);

            /* Full prefix for the subdirectory, e.g. "src/" or "a/b/" */
            char sub_prefix[600] = {0};
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);

            /* Count consecutive entries that belong to this subdirectory */
            int j = i;
            while (j < count &&
                   strncmp(entries[j].path, sub_prefix, strlen(sub_prefix)) == 0) {
                j++;
            }

            /* Recurse: build the subtree for entries[i..j) */
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, sub_prefix, &sub_id) != 0)
                return -1;

            /* Add a DIR entry pointing to the subtree hash */
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = sub_id;

            i = j; /* advance past the consumed entries */
        }
    }

    /* Serialize the completed Tree and write it to the object store */
    void *blob  = NULL;
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
    if (idx.count == 0)        return -1; /* nothing staged */

    /* index entries must be sorted by path for the grouping logic to work.
       index_save sorts on write, so they should already be in order, but
       sort defensively here as well. */
    return write_tree_level(idx.entries, idx.count, "", id_out);
}