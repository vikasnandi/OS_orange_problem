// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── Helper: map ObjectType enum to string ───────────────────────────────────

static const char *type_to_str(ObjectType type) {
    switch (type) {
        case OBJ_BLOB:   return "blob";
        case OBJ_TREE:   return "tree";
        case OBJ_COMMIT: return "commit";
        default:         return NULL;
    }
}

static ObjectType str_to_type(const char *s) {
    if (strcmp(s, "blob")   == 0) return OBJ_BLOB;
    if (strcmp(s, "tree")   == 0) return OBJ_TREE;
    if (strcmp(s, "commit") == 0) return OBJ_COMMIT;
    return -1; /* unknown */
}

// ─── TODO: Implemented ───────────────────────────────────────────────────────

// Write an object to the store.
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t size, ObjectID *id_out) {
    const char *type_str = type_to_str(type);
    if (!type_str) return -1;

    // 1. Build full object: "<type> <size>\0<data>"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, size);
    header_len++;   /* include the '\0' terminator */

    size_t total_size = (size_t)header_len + size;
    unsigned char *buffer = malloc(total_size);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    memcpy(buffer + header_len, data, size);

    // 2. Compute SHA-256 of full object (header + data)
    compute_hash(buffer, total_size, id_out);

    // 3. Deduplication: if object already exists, we're done
    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    // 4. Create shard directory (.pes/objects/XX/)
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(OBJECTS_DIR, 0755);   /* no-op if exists */
    mkdir(shard_dir, 0755);

    // 5. Final object path
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    // 6. Write to a temp file in the same shard directory
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_XXXXXX", shard_dir);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    ssize_t written = write(fd, buffer, total_size);
    free(buffer);

    if (written < 0 || (size_t)written != total_size) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // 7. fsync the temp file
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    // 8. Atomic rename to final path
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // 9. fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// Read an object from the store.
// Returns 0 on success, -1 on error.
int object_read(const ObjectID *id, ObjectType *type_out,
                void **data_out, size_t *len_out) {
    // 1. Build file path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size_signed = ftell(f);
    rewind(f);

    if (file_size_signed < 0) { fclose(f); return -1; }
    size_t file_size = (size_t)file_size_signed;

    unsigned char *buffer = malloc(file_size);
    if (!buffer) { fclose(f); return -1; }

    if (fread(buffer, 1, file_size, f) != file_size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 3. Verify integrity: recompute hash and compare to *id
    ObjectID computed;
    compute_hash(buffer, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;     /* corrupt object */
    }

    // 4. Parse header — find the '\0' that ends it
    char *header_end = memchr(buffer, '\0', file_size);
    if (!header_end) { free(buffer); return -1; }

    char type_str[16] = {0};
    size_t data_size = 0;
    sscanf((char *)buffer, "%15s %zu", type_str, &data_size);

    // 5. Map type string to ObjectType enum
    ObjectType otype = str_to_type(type_str);
    if ((int)otype < 0) { free(buffer); return -1; }
    *type_out = otype;

    // 6. Copy data portion (everything after the '\0')
    size_t header_len = (size_t)(header_end - (char *)buffer) + 1;
    /* +1 so callers can safely treat the data as a C string */
    *data_out = malloc(data_size + 1);
    if (!(*data_out)) { free(buffer); return -1; }

    memcpy(*data_out, buffer + header_len, data_size);
    ((char *)*data_out)[data_size] = '\0';
    *len_out = data_size;

    free(buffer);
    return 0;
}