// Host test for the volatile (PSRAM) photo store: retention limits, allocation failure and
// reference-counted readers racing delete/prune.
//
//   cc -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -I main
//      tests/test_photo_store.c main/photo_store.c -o /tmp/mypi-photo-store && /tmp/mypi-photo-store
//   (also run with -fsanitize=thread instead of address,undefined for the concurrency part)

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "photo_store.h"

static int g_allocs, g_frees, g_fail_next;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_count_lock = PTHREAD_MUTEX_INITIALIZER;

static void *t_alloc(size_t n, void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&g_count_lock);
    bool fail = g_fail_next > 0;
    if (fail) g_fail_next--;
    else g_allocs++;
    pthread_mutex_unlock(&g_count_lock);
    return fail ? NULL : malloc(n);
}

static void t_free(void *p, void *ctx)
{
    (void)ctx;
    // Poison freed photos so a use-after-free shows up even without ASan.
    memset(p, 0xA5, 16);
    free(p);
    pthread_mutex_lock(&g_count_lock);
    g_frees++;
    pthread_mutex_unlock(&g_count_lock);
}

static void t_lock(void *ctx) { (void)ctx; pthread_mutex_lock(&g_lock); }
static void t_unlock(void *ctx) { (void)ctx; pthread_mutex_unlock(&g_lock); }

static const photo_store_ops_t OPS = {t_alloc, t_free, t_lock, t_unlock, NULL};

static int live(void) { return g_allocs - g_frees; }

static void fill(uint8_t *buf, size_t n, uint8_t v) { memset(buf, v, n); }

static void test_limits(void)
{
    static photo_store_t s;
    photo_store_init(&s, &OPS);
    uint8_t buf[1000];
    char name[PHOTO_STORE_NAME_LEN];
    photo_store_stats_t st;

    fill(buf, sizeof(buf), 1);
    assert(photo_store_add(&s, buf, 0, 3, 10000, name) == PHOTO_STORE_ERR_ARG && name[0] == '\0');
    assert(photo_store_add(&s, buf, 100, 0, 10000, name) == PHOTO_STORE_ERR_ARG);
    assert(photo_store_add(&s, buf, 1000, 3, 999, name) == PHOTO_STORE_ERR_TOO_LARGE);
    assert(live() == 0);

    // Count limit: the oldest photo is pruned to make room.
    for (int i = 1; i <= 4; i++) {
        fill(buf, sizeof(buf), (uint8_t)i);
        assert(photo_store_add(&s, buf, 100, 3, 10000, name) == PHOTO_STORE_OK);
        char want[PHOTO_STORE_NAME_LEN];
        snprintf(want, sizeof(want), "IMG%05d.JPG", i);
        assert(strcmp(name, want) == 0);
    }
    photo_store_info_t list[8];
    int n = photo_store_list(&s, list, 8);
    assert(n == 3 && strcmp(list[0].name, "IMG00004.JPG") == 0 && strcmp(list[2].name, "IMG00002.JPG") == 0);
    assert(list[0].size == 100);
    assert(photo_store_get(&s, "IMG00001.JPG") == NULL);
    photo_store_stats(&s, &st);
    assert(st.count == 3 && st.used_bytes == 300 && st.evicted == 1 && live() == 3);

    // Byte budget: a 700-byte photo in a 900-byte budget leaves room for two 100-byte photos.
    fill(buf, sizeof(buf), 9);
    assert(photo_store_add(&s, buf, 700, 10, 900, name) == PHOTO_STORE_OK && strcmp(name, "IMG00005.JPG") == 0);
    photo_store_stats(&s, &st);
    assert(st.count == 3 && st.used_bytes == 900 && live() == 3);
    photo_blob_t *b = photo_store_get(&s, "IMG00005.JPG");
    assert(b && photo_blob_len(b) == 700 && photo_blob_data(b)[699] == 9);
    photo_store_put(&s, b);

    // Allocation failure never evicts existing photos.
    g_fail_next = 1;
    assert(photo_store_add(&s, buf, 500, 1, 900, name) == PHOTO_STORE_ERR_NO_MEM);
    photo_store_stats(&s, &st);
    assert(st.count == 3 && st.used_bytes == 900);

    // Shrinking limits (settings change) trims the oldest photos immediately.
    assert(photo_store_trim(&s, 10, 750) == 2);
    n = photo_store_list(&s, list, 8);
    assert(n == 1 && strcmp(list[0].name, "IMG00005.JPG") == 0 && live() == 1);
    assert(photo_store_trim(&s, 10, 750) == 0);

    assert(photo_store_delete(&s, "IMG00005.JPG") == PHOTO_STORE_OK);
    assert(photo_store_delete(&s, "IMG00005.JPG") == PHOTO_STORE_ERR_NOT_FOUND);
    assert(photo_store_delete(&s, NULL) == PHOTO_STORE_ERR_NOT_FOUND);
    assert(live() == 0);
    photo_store_clear(&s);
}

static void test_reader_outlives_delete(void)
{
    static photo_store_t s;
    photo_store_init(&s, &OPS);
    uint8_t buf[256];
    char name[PHOTO_STORE_NAME_LEN];
    fill(buf, sizeof(buf), 0x42);
    assert(photo_store_add(&s, buf, sizeof(buf), 5, 4096, name) == PHOTO_STORE_OK);
    photo_blob_t *b = photo_store_get(&s, name);
    assert(b);
    assert(photo_store_delete(&s, name) == PHOTO_STORE_OK);
    photo_store_stats_t st;
    photo_store_stats(&s, &st);
    assert(st.count == 0 && st.used_bytes == 0 && st.pending_bytes == 256 && live() == 1);
    for (size_t i = 0; i < sizeof(buf); i++) assert(photo_blob_data(b)[i] == 0x42);
    photo_store_put(&s, b);
    photo_store_stats(&s, &st);
    assert(st.pending_bytes == 0 && live() == 0);

    // Pruned while a reader holds it.
    assert(photo_store_add(&s, buf, sizeof(buf), 1, 4096, name) == PHOTO_STORE_OK);
    b = photo_store_get(&s, name);
    assert(photo_store_add(&s, buf, sizeof(buf), 1, 4096, name) == PHOTO_STORE_OK);
    photo_store_stats(&s, &st);
    assert(st.count == 1 && st.pending_bytes == 256 && live() == 2);
    assert(photo_blob_data(b)[0] == 0x42);
    photo_store_put(&s, b);
    photo_store_clear(&s);
    assert(live() == 0);
}

static void test_name_wrap(void)
{
    static photo_store_t s;
    photo_store_init(&s, &OPS);
    uint8_t buf[4] = {1, 2, 3, 4};
    char name[PHOTO_STORE_NAME_LEN];
    assert(photo_store_add(&s, buf, 4, 5, 4096, name) == PHOTO_STORE_OK && strcmp(name, "IMG00001.JPG") == 0);
    s.next_seq = PHOTO_STORE_SEQ_MAX;
    assert(photo_store_add(&s, buf, 4, 5, 4096, name) == PHOTO_STORE_OK && strcmp(name, "IMG99999.JPG") == 0);
    // Wraps to 1, which is still listed, so 2 is used.
    assert(photo_store_add(&s, buf, 4, 5, 4096, name) == PHOTO_STORE_OK && strcmp(name, "IMG00002.JPG") == 0);
    photo_store_clear(&s);
    assert(live() == 0);
}

// Concurrency: readers copy photos while a writer keeps adding (pruning) and deleting.
static photo_store_t g_store;
static atomic_int g_stop;

static void *reader(void *arg)
{
    (void)arg;
    photo_store_info_t list[8];
    unsigned long reads = 0;
    while (!g_stop) {
        int n = photo_store_list(&g_store, list, 8);
        for (int i = 0; i < n; i++) {
            photo_blob_t *b = photo_store_get(&g_store, list[i].name);
            if (!b) continue;   // Deleted between list and get: fine
            const uint8_t *d = photo_blob_data(b);
            size_t len = photo_blob_len(b);
            // Each photo is filled with one byte value; a freed/reused block would differ.
            for (size_t k = 1; k < len; k++) assert(d[k] == d[0]);
            reads++;
            photo_store_put(&g_store, b);
        }
    }
    return (void *)reads;
}

static void test_concurrency(void)
{
    photo_store_init(&g_store, &OPS);
    pthread_t th[3];
    g_stop = 0;
    for (int i = 0; i < 3; i++) assert(pthread_create(&th[i], NULL, reader, NULL) == 0);
    uint8_t buf[4096];
    char name[PHOTO_STORE_NAME_LEN];
    for (int i = 0; i < 3000; i++) {
        size_t len = 512 + (size_t)(i * 37) % 3500;
        fill(buf, len, (uint8_t)i);
        assert(photo_store_add(&g_store, buf, len, 4, 12000, name) == PHOTO_STORE_OK);
        if (i % 7 == 0) photo_store_delete(&g_store, name);
        if (i % 101 == 0) photo_store_trim(&g_store, 2, 6000);
    }
    g_stop = 1;
    unsigned long total = 0;
    for (int i = 0; i < 3; i++) {
        void *r;
        pthread_join(th[i], &r);
        total += (unsigned long)r;
    }
    photo_store_stats_t st;
    photo_store_stats(&g_store, &st);
    assert(st.pending_bytes == 0 && st.count <= 4 && st.used_bytes <= 12000);
    photo_store_clear(&g_store);
    assert(live() == 0);
    printf("concurrent reads: %lu\n", total);
}

int main(void)
{
    test_limits();
    test_reader_outlives_delete();
    test_name_wrap();
    test_concurrency();
    puts("photo store tests passed");
    return 0;
}
