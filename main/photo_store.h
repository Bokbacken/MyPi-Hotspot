#pragma once

// Volatile photo store: JPEG photos live only in RAM (on the device: external PSRAM, see camera.c).
// Nothing is written to flash, so the store is empty after every reboot.
//
// Portable C without ESP-IDF dependencies (host-tested in tests/test_photo_store.c): the caller
// supplies the allocator (PSRAM on the device) and the lock.
//
// Lifetimes: every photo is one reference-counted allocation. The index owns one reference;
// photo_store_get() hands out another one, so a download keeps reading valid memory even if the
// photo is deleted, pruned or the limits shrink meanwhile. The memory is freed when the last
// reference is dropped (photo_store_put()).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHOTO_STORE_MAX         500     // Upper bound of the max_photos setting
#define PHOTO_STORE_NAME_LEN    13      // "IMGnnnnn.JPG" + NUL
#define PHOTO_STORE_SEQ_MAX     99999

typedef struct photo_blob photo_blob_t;

typedef struct {
    void *(*alloc)(size_t size, void *ctx);     // NULL = out of memory
    void (*free)(void *ptr, void *ctx);
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} photo_store_ops_t;

typedef struct {
    const photo_store_ops_t *ops;
    photo_blob_t *items[PHOTO_STORE_MAX];       // Oldest first
    int count;
    size_t used_bytes;      // JPEG bytes of listed photos (counted against the budget)
    size_t pending_bytes;   // Deleted/pruned photos still referenced by a reader
    uint32_t next_seq;
    uint32_t added, evicted;
} photo_store_t;

typedef enum {
    PHOTO_STORE_OK = 0,
    PHOTO_STORE_ERR_ARG,        // Empty photo / bad limits
    PHOTO_STORE_ERR_TOO_LARGE,  // A single photo exceeds the byte budget
    PHOTO_STORE_ERR_NO_MEM,     // Allocation failed (nothing was evicted)
    PHOTO_STORE_ERR_NOT_FOUND,
} photo_store_err_t;

typedef struct {
    char name[PHOTO_STORE_NAME_LEN];
    uint32_t size;
} photo_store_info_t;

typedef struct {
    int count;
    size_t used_bytes, pending_bytes;
    uint32_t added, evicted;
} photo_store_stats_t;

void photo_store_init(photo_store_t *s, const photo_store_ops_t *ops);

/**
 * Copy a JPEG into a new allocation and list it as the newest photo. The oldest photos are then
 * evicted until at most max_photos remain and the listed bytes fit budget_bytes. The allocation
 * happens before anything is evicted, so a failed add never loses existing photos.
 */
photo_store_err_t photo_store_add(photo_store_t *s, const uint8_t *data, size_t len, int max_photos,
                                  size_t budget_bytes, char name_out[PHOTO_STORE_NAME_LEN]);

/** Evict the oldest photos until the limits hold (settings change). Returns the evicted count. */
int photo_store_trim(photo_store_t *s, int max_photos, size_t budget_bytes);

/** New reference to a photo, or NULL. Release it with photo_store_put(). */
photo_blob_t *photo_store_get(photo_store_t *s, const char *name);
void photo_store_put(photo_store_t *s, photo_blob_t *blob);
const uint8_t *photo_blob_data(const photo_blob_t *blob);
size_t photo_blob_len(const photo_blob_t *blob);

photo_store_err_t photo_store_delete(photo_store_t *s, const char *name);

/** Listed photos, newest first; returns the number written (<= max). */
int photo_store_list(photo_store_t *s, photo_store_info_t *out, int max);

void photo_store_stats(photo_store_t *s, photo_store_stats_t *out);

/** Drop every listed photo (readers keep their references). */
void photo_store_clear(photo_store_t *s);
