#include "photo_store.h"

#include <stdio.h>
#include <string.h>

struct photo_blob {
    uint32_t refs;          // Protected by the store lock
    bool listed;
    char name[PHOTO_STORE_NAME_LEN];
    size_t len;
    uint8_t data[];
};

static void lock(photo_store_t *s)
{
    if (s->ops->lock) s->ops->lock(s->ops->ctx);
}

static void unlock(photo_store_t *s)
{
    if (s->ops->unlock) s->ops->unlock(s->ops->ctx);
}

void photo_store_init(photo_store_t *s, const photo_store_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->next_seq = 1;
}

static int find_locked(const photo_store_t *s, const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->items[i]->name, name) == 0) return i;
    }
    return -1;
}

/** Remove index entry i and drop the index reference; returns the blob if it must be freed. */
static photo_blob_t *unlist_locked(photo_store_t *s, int i)
{
    photo_blob_t *b = s->items[i];
    memmove(&s->items[i], &s->items[i + 1], (size_t)(s->count - i - 1) * sizeof(s->items[0]));
    s->count--;
    s->items[s->count] = NULL;
    s->used_bytes -= b->len;
    b->listed = false;
    if (--b->refs == 0) return b;
    s->pending_bytes += b->len;     // A reader still holds it; freed by photo_store_put()
    return NULL;
}

/** Evict the oldest photos until the limits hold for `extra` more bytes (lock held). Freeing a
 *  PSRAM block under the store mutex is fine and keeps callers' stacks small. */
static void evict_locked(photo_store_t *s, int max_count, size_t budget, size_t extra)
{
    while (s->count > 0 && (s->count > max_count || s->used_bytes + extra > budget)) {
        photo_blob_t *b = unlist_locked(s, 0);
        s->evicted++;
        if (b) s->ops->free(b, s->ops->ctx);
    }
}

photo_store_err_t photo_store_add(photo_store_t *s, const uint8_t *data, size_t len, int max_photos,
                                  size_t budget_bytes, char name_out[PHOTO_STORE_NAME_LEN])
{
    if (name_out) name_out[0] = '\0';
    if (!s || !data || len == 0 || max_photos < 1 || budget_bytes == 0) return PHOTO_STORE_ERR_ARG;
    if (max_photos > PHOTO_STORE_MAX) max_photos = PHOTO_STORE_MAX;
    if (len > budget_bytes || len > SIZE_MAX - sizeof(photo_blob_t)) return PHOTO_STORE_ERR_TOO_LARGE;

    photo_blob_t *b = s->ops->alloc(sizeof(*b) + len, s->ops->ctx);
    if (!b) return PHOTO_STORE_ERR_NO_MEM;
    memset(b, 0, sizeof(*b));
    memcpy(b->data, data, len);
    b->len = len;
    b->refs = 1;
    b->listed = true;

    lock(s);
    evict_locked(s, max_photos - 1, budget_bytes, len);
    // Sequence numbers restart at 1 after a reboot; skip names still in use after a wrap.
    for (int tries = 0; tries <= PHOTO_STORE_SEQ_MAX; tries++) {
        if (s->next_seq < 1 || s->next_seq > PHOTO_STORE_SEQ_MAX) s->next_seq = 1;
        unsigned seq = (unsigned)(s->next_seq++ % (PHOTO_STORE_SEQ_MAX + 1));
        snprintf(b->name, sizeof(b->name), "IMG%05u.JPG", seq);
        if (find_locked(s, b->name) < 0) break;
    }
    s->items[s->count++] = b;
    s->used_bytes += len;
    s->added++;
    if (name_out) memcpy(name_out, b->name, PHOTO_STORE_NAME_LEN);
    unlock(s);
    return PHOTO_STORE_OK;
}

int photo_store_trim(photo_store_t *s, int max_photos, size_t budget_bytes)
{
    if (!s || max_photos < 1) return 0;
    lock(s);
    int before = s->count;
    evict_locked(s, max_photos, budget_bytes, 0);
    int evicted = before - s->count;
    unlock(s);
    return evicted;
}

photo_blob_t *photo_store_get(photo_store_t *s, const char *name)
{
    if (!s) return NULL;
    lock(s);
    int i = find_locked(s, name);
    photo_blob_t *b = i >= 0 ? s->items[i] : NULL;
    if (b) b->refs++;
    unlock(s);
    return b;
}

void photo_store_put(photo_store_t *s, photo_blob_t *b)
{
    if (!s || !b) return;
    lock(s);
    bool last = --b->refs == 0;
    if (last && !b->listed) s->pending_bytes -= b->len;
    unlock(s);
    if (last) s->ops->free(b, s->ops->ctx);
}

const uint8_t *photo_blob_data(const photo_blob_t *b) { return b ? b->data : NULL; }
size_t photo_blob_len(const photo_blob_t *b) { return b ? b->len : 0; }

photo_store_err_t photo_store_delete(photo_store_t *s, const char *name)
{
    if (!s) return PHOTO_STORE_ERR_ARG;
    lock(s);
    int i = find_locked(s, name);
    photo_blob_t *b = i >= 0 ? unlist_locked(s, i) : NULL;
    unlock(s);
    if (i < 0) return PHOTO_STORE_ERR_NOT_FOUND;
    if (b) s->ops->free(b, s->ops->ctx);
    return PHOTO_STORE_OK;
}

int photo_store_list(photo_store_t *s, photo_store_info_t *out, int max)
{
    if (!s || !out || max <= 0) return 0;
    lock(s);
    int n = 0;
    for (int i = s->count - 1; i >= 0 && n < max; i--, n++) {
        memcpy(out[n].name, s->items[i]->name, PHOTO_STORE_NAME_LEN);
        out[n].size = (uint32_t)s->items[i]->len;
    }
    unlock(s);
    return n;
}

void photo_store_stats(photo_store_t *s, photo_store_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;
    lock(s);
    out->count = s->count;
    out->used_bytes = s->used_bytes;
    out->pending_bytes = s->pending_bytes;
    out->added = s->added;
    out->evicted = s->evicted;
    unlock(s);
}

void photo_store_clear(photo_store_t *s)
{
    if (!s) return;
    lock(s);
    while (s->count > 0) {
        photo_blob_t *b = unlist_locked(s, 0);
        if (b) s->ops->free(b, s->ops->ctx);
    }
    unlock(s);
}
