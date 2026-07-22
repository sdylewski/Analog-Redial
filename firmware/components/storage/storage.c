#include "storage.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *TAG = "storage";
#define MOUNT_POINT "/assets"          /* writable: recordings + uploads */
#define BUNDLED_MOUNT_POINT "/bundled" /* read-only-in-spirit: baked in at build time */

static bool s_bundled_mounted;

static char full_path(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, MOUNT_POINT "/%s", name);
    return 0;
}

static void bundled_path(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, BUNDLED_MOUNT_POINT "/%s", name);
}

esp_err_t storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 20,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "mounted %s: %u/%u bytes used (recording cap %u)",
             MOUNT_POINT, (unsigned)used, (unsigned)total,
             (unsigned)CONFIG_STORAGE_RECORDING_CAP_BYTES);

    /* Baked-in defaults (firmware/assets/ at build time -- see
     * spiffs_create_partition_image in main/CMakeLists.txt). Never
     * format-on-fail: this partition is meant to be built by the project,
     * not written to at runtime, so a mount failure just means "no bundled
     * assets this build" rather than something to paper over by erasing it. */
    esp_vfs_spiffs_conf_t bconf = {
        .base_path = BUNDLED_MOUNT_POINT,
        .partition_label = "bundled",
        .max_files = 6,
        .format_if_mount_failed = false,
    };
    err = esp_vfs_spiffs_register(&bconf);
    if (err == ESP_OK) {
        s_bundled_mounted = true;
        size_t btotal = 0, bused = 0;
        esp_spiffs_info("bundled", &btotal, &bused);
        ESP_LOGI(TAG, "mounted %s: %u/%u bytes (baked in at build time)",
                 BUNDLED_MOUNT_POINT, (unsigned)bused, (unsigned)btotal);
    } else {
        ESP_LOGW(TAG, "bundled assets partition not mounted: %s (nothing baked in this build?)",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

size_t storage_used_bytes(void)
{
    size_t total = 0;
    DIR *d = opendir(MOUNT_POINT);
    if (!d) return 0;
    struct dirent *de;
    char path[288];
    struct stat st;
    while ((de = readdir(d)) != NULL) {
        full_path(path, sizeof(path), de->d_name);
        if (stat(path, &st) == 0) total += (size_t)st.st_size;
    }
    closedir(d);
    return total;
}

size_t storage_cap_bytes(void) { return CONFIG_STORAGE_RECORDING_CAP_BYTES; }
bool   storage_using_sd(void) { return false; }

/* Audio assets (fun clips, the operator prompt) share this partition with
 * user recordings but aren't recordings themselves -- they're meant to
 * outlive the recording cap, not get silently deleted the first time someone
 * fills it up leaving messages. Only actual recordings (rec_*.wav from 211,
 * cb_*.wav temp files from 212-215) are fair game for automatic eviction;
 * anything else has to be deleted on purpose (web UI / storage_delete()). */
static bool is_protected_asset(const char *name)
{
    return strcmp(name, "operator.wav") == 0 || strncmp(name, "clip_", 5) == 0;
}

/* Deletes the single oldest *evictable* file (by mtime), skipping protected
 * assets. Returns its size, or 0 if there was nothing evictable. */
static size_t evict_oldest(void)
{
    DIR *d = opendir(MOUNT_POINT);
    if (!d) return 0;
    struct dirent *de;
    char path[288], oldest_path[288] = {0};
    struct stat st, oldest_st = { .st_mtime = 0 };
    bool found = false;

    while ((de = readdir(d)) != NULL) {
        if (is_protected_asset(de->d_name)) continue;
        full_path(path, sizeof(path), de->d_name);
        if (stat(path, &st) != 0) continue;
        if (!found || st.st_mtime < oldest_st.st_mtime) {
            found = true;
            oldest_st = st;
            strncpy(oldest_path, path, sizeof(oldest_path) - 1);
        }
    }
    closedir(d);
    if (!found) return 0;
    size_t sz = (size_t)oldest_st.st_size;
    ESP_LOGI(TAG, "evicting %s (%u bytes) to make room", oldest_path, (unsigned)sz);
    unlink(oldest_path);
    return sz;
}

esp_err_t storage_write_begin(const char *name, size_t expected_len, storage_writer_t *w)
{
    w->file = NULL;
    w->path[0] = '\0';
    if (expected_len > (size_t)CONFIG_STORAGE_RECORDING_CAP_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    full_path(w->path, sizeof(w->path), name);

    /* Overwriting an existing file of the same name: remove it first so its
     * old size isn't double-counted against the cap. */
    struct stat st;
    if (stat(w->path, &st) == 0) unlink(w->path);

    size_t used = storage_used_bytes();
    int guard = 64; /* bounded -- never loop forever if the dir is somehow stuck */
    while (used + expected_len > (size_t)CONFIG_STORAGE_RECORDING_CAP_BYTES && guard-- > 0) {
        size_t freed = evict_oldest();
        if (freed == 0) break; /* nothing left to evict */
        used -= freed;
    }
    if (used + expected_len > (size_t)CONFIG_STORAGE_RECORDING_CAP_BYTES) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(w->path, "wb");
    if (!f) return ESP_FAIL;
    w->file = f;
    return ESP_OK;
}

esp_err_t storage_write_chunk(storage_writer_t *w, const void *data, size_t len)
{
    if (!w->file) return ESP_ERR_INVALID_STATE;
    size_t written = fwrite(data, 1, len, (FILE *)w->file);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_write_end(storage_writer_t *w)
{
    if (!w->file) return ESP_ERR_INVALID_STATE;
    int rc = fclose((FILE *)w->file);
    w->file = NULL;
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

void storage_write_abort(storage_writer_t *w)
{
    if (w->file) {
        fclose((FILE *)w->file);
        w->file = NULL;
    }
    if (w->path[0]) unlink(w->path);
}

esp_err_t storage_write(const char *name, const void *data, size_t len)
{
    storage_writer_t w;
    esp_err_t err = storage_write_begin(name, len, &w);
    if (err != ESP_OK) return err;
    err = storage_write_chunk(&w, data, len);
    if (err != ESP_OK) {
        storage_write_abort(&w);
        return err;
    }
    return storage_write_end(&w);
}

esp_err_t storage_read(const char *name, void *buf, size_t buf_len, size_t *out_len)
{
    char path[288];
    full_path(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    if (!f && s_bundled_mounted) {
        bundled_path(path, sizeof(path), name);
        f = fopen(path, "rb");
    }
    if (!f) return ESP_ERR_NOT_FOUND;
    size_t n = fread(buf, 1, buf_len, f);
    fclose(f);
    if (out_len) *out_len = n;
    return ESP_OK;
}

esp_err_t storage_read_open(const char *name, storage_reader_t *r)
{
    /* Writable storage first, so an upload/recording of the same name
     * overrides a bundled default without a rebuild. */
    char path[288];
    full_path(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    if (!f && s_bundled_mounted) {
        bundled_path(path, sizeof(path), name);
        f = fopen(path, "rb");
    }
    if (!f) return ESP_ERR_NOT_FOUND;
    r->file = f;
    return ESP_OK;
}

esp_err_t storage_read_seek(storage_reader_t *r, size_t offset)
{
    if (!r->file) return ESP_ERR_INVALID_STATE;
    return (fseek((FILE *)r->file, (long)offset, SEEK_SET) == 0) ? ESP_OK : ESP_FAIL;
}

size_t storage_read_chunk(storage_reader_t *r, void *buf, size_t len)
{
    if (!r->file) return 0;
    return fread(buf, 1, len, (FILE *)r->file);
}

void storage_read_close(storage_reader_t *r)
{
    if (r->file) {
        fclose((FILE *)r->file);
        r->file = NULL;
    }
}

esp_err_t storage_delete(const char *name)
{
    char path[288];
    full_path(path, sizeof(path), name);
    return (unlink(path) == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_patch(const char *name, size_t offset, const void *data, size_t len)
{
    char path[288];
    full_path(path, sizeof(path), name);
    FILE *f = fopen(path, "r+b");
    if (!f) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_OK;
    if (fseek(f, (long)offset, SEEK_SET) != 0 || fwrite(data, 1, len, f) != len) {
        err = ESP_FAIL;
    }
    fclose(f);
    return err;
}

static int cmp_mtime(const void *a, const void *b)
{
    const struct stat *sa = a, *sb = b;
    if (sa->st_mtime < sb->st_mtime) return -1;
    if (sa->st_mtime > sb->st_mtime) return 1;
    return 0;
}

static size_t list_dir(bool bundled, storage_entry_t *out, size_t max_entries)
{
    DIR *d = opendir(bundled ? BUNDLED_MOUNT_POINT : MOUNT_POINT);
    if (!d) return 0;

    /* Small, bounded (recordings are capped in the low hundreds of KB-MB
     * range at most): collect name+stat pairs, sort by mtime, then copy out
     * up to max_entries. Simple insertion sort -- entry counts here are
     * tiny, not worth pulling in qsort machinery for structs of two arrays. */
    struct dirent *de;
    char path[288];
    struct stat st;
    size_t n = 0;
    const size_t cap = 64;
    /* Heap, not stack -- 64 * (32 + sizeof(struct stat)) is too big a frame
     * for a console/httpd worker task stack. */
    char (*names)[STORAGE_MAX_NAME] = malloc(cap * STORAGE_MAX_NAME);
    struct stat *stats = malloc(cap * sizeof(struct stat));
    if (!names || !stats) {
        free(names);
        free(stats);
        closedir(d);
        return 0;
    }

    while ((de = readdir(d)) != NULL && n < cap) {
        /* Literal-prefixed format (not a runtime mount string) so the
         * compiler can bound the worst case -- same pattern as full_path(). */
        if (bundled) snprintf(path, sizeof(path), BUNDLED_MOUNT_POINT "/%s", de->d_name);
        else         snprintf(path, sizeof(path), MOUNT_POINT "/%s", de->d_name);
        if (stat(path, &st) != 0) continue;
        strncpy(names[n], de->d_name, STORAGE_MAX_NAME - 1);
        names[n][STORAGE_MAX_NAME - 1] = 0;
        stats[n] = st;
        n++;
    }
    closedir(d);

    /* insertion sort by mtime, keep names[]/stats[] in lockstep */
    for (size_t i = 1; i < n; i++) {
        struct stat key_st = stats[i];
        char key_name[STORAGE_MAX_NAME];
        strncpy(key_name, names[i], STORAGE_MAX_NAME);
        int j = (int)i - 1;
        while (j >= 0 && cmp_mtime(&stats[j], &key_st) > 0) {
            stats[j + 1] = stats[j];
            strncpy(names[j + 1], names[j], STORAGE_MAX_NAME);
            j--;
        }
        stats[j + 1] = key_st;
        strncpy(names[j + 1], key_name, STORAGE_MAX_NAME);
    }

    size_t copied = (n < max_entries) ? n : max_entries;
    for (size_t i = 0; i < copied; i++) {
        strncpy(out[i].name, names[i], STORAGE_MAX_NAME - 1);
        out[i].name[STORAGE_MAX_NAME - 1] = 0;
        out[i].size = (size_t)stats[i].st_size;
    }
    free(names);
    free(stats);
    return copied;
}

size_t storage_list(storage_entry_t *out, size_t max_entries)
{
    return list_dir(false, out, max_entries);
}

size_t storage_list_bundled(storage_entry_t *out, size_t max_entries)
{
    if (!s_bundled_mounted) return 0;
    return list_dir(true, out, max_entries);
}
