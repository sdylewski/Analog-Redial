#include "storage.h"
#include "sdcard.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

static const char *TAG = "storage";
#define MOUNT_POINT "/assets"          /* writable: recordings + uploads */
#define SD_MOUNT_POINT "/sd"           /* mirrors sdcard.c's SD_MOUNT -- kept as a separate
                                        * literal rather than a shared header so this component
                                        * doesn't need to reach into sdcard's internals, just its
                                        * public sdcard_mounted()/sdcard_capacity() */
#define BUNDLED_MOUNT_POINT "/bundled" /* read-only-in-spirit: baked in at build time */

static bool s_bundled_mounted;

static void full_path(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, MOUNT_POINT "/%s", name);
}

static void sd_full_path(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, SD_MOUNT_POINT "/%s", name);
}

static void bundled_path(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, BUNDLED_MOUNT_POINT "/%s", name);
}

/* Saturate a 64-bit byte count into this chip's 32-bit size_t rather than
 * silently wrapping -- only matters for the legacy primary-backend API
 * (storage_cap_bytes() et al) once an SD card past ~4 GB is primary; the
 * dedicated *_sd() getters report the real uint64_t value instead. */
static size_t saturate_size(uint64_t v)
{
    return (v > (uint64_t)SIZE_MAX) ? SIZE_MAX : (size_t)v;
}

/* Tries the primary backend (SD if mounted, else internal), then the other
 * one, writing whichever path actually has `name` into `out`. Used by reads/
 * delete/patch so a file made before a card was inserted (or after one was
 * removed) doesn't just vanish -- see storage.h. Returns false if neither
 * backend has it (caller decides whether to also try bundled). */
static bool path_exists(const char *path)
{
    /* stat(), not access() -- list_dir() (which every existing-file listing
     * in this component goes through) already relies on stat() succeeding
     * for a file to be listed at all, so if a listed file's existence check
     * here disagrees, access() is the one that's wrong, not the file.
     * Confirmed live: several uploaded clips listed fine (stat-based) but
     * resolve_existing_path()'s old access()-based check couldn't find them
     * -- delete and even plain playback (/media/<name>) both silently 500'd
     * for files that clearly existed. */
    struct stat st;
    return stat(path, &st) == 0;
}

static bool resolve_existing_path(const char *name, char *out, size_t out_len)
{
    bool sd = sdcard_mounted();
    if (sd) {
        sd_full_path(out, out_len, name);
        if (path_exists(out)) return true;
        full_path(out, out_len, name);
        if (path_exists(out)) return true;
    } else {
        full_path(out, out_len, name);
        if (path_exists(out)) return true;
    }
    return false;
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

static size_t dir_used_bytes(const char *mount)
{
    size_t total = 0;
    DIR *d = opendir(mount);
    if (!d) return 0;
    struct dirent *de;
    char path[288];
    struct stat st;
    while ((de = readdir(d)) != NULL) {
        snprintf(path, sizeof(path), "%s/%s", mount, de->d_name);
        if (stat(path, &st) == 0) total += (size_t)st.st_size;
    }
    closedir(d);
    return total;
}

size_t storage_used_bytes_internal(void) { return dir_used_bytes(MOUNT_POINT); }
size_t storage_cap_bytes_internal(void) { return CONFIG_STORAGE_RECORDING_CAP_BYTES; }

/* FATFS's own accounting, not a directory walk -- faster and accounts for
 * filesystem overhead a plain file-size sum would miss. 0/0/false whenever
 * nothing's actually mounted (not configured for SD, or configured but no
 * card inserted right now). */
uint64_t storage_used_bytes_sd(void)
{
    if (!sdcard_mounted()) return 0;
    uint64_t total = 0, free_b = 0;
    if (sdcard_capacity(&total, &free_b) != ESP_OK) return 0;
    return (total > free_b) ? (total - free_b) : 0;
}

uint64_t storage_cap_bytes_sd(void)
{
    if (!sdcard_mounted()) return 0;
    uint64_t total = 0, free_b = 0;
    return (sdcard_capacity(&total, &free_b) == ESP_OK) ? total : 0;
}

bool storage_sd_present(void) { return sdcard_mounted(); }

size_t storage_used_bytes(void)
{
    return sdcard_mounted() ? saturate_size(storage_used_bytes_sd()) : storage_used_bytes_internal();
}

size_t storage_cap_bytes(void)
{
    return sdcard_mounted() ? saturate_size(storage_cap_bytes_sd()) : storage_cap_bytes_internal();
}

bool storage_using_sd(void) { return sdcard_mounted(); }

/* Audio assets (fun clips, the operator prompt, anything uploaded and wired
 * to a dial code) share this partition with user recordings but aren't
 * recordings themselves -- they're meant to outlive the recording cap, not
 * get silently deleted the first time someone fills it up leaving messages.
 * Only actual recordings (rec_*.wav from 211) are fair game for automatic
 * eviction; anything else has to be deleted on purpose (web UI /
 * storage_delete()). Denylist rather than an allowlist of asset names, since
 * clip filenames are no longer a fixed naming convention (see dialplan's
 * clip-entry table) -- only the recording prefix is known in advance.
 * (cb_*.wav, the temp file from the archived "record + callback" feature,
 * used to be protected here too -- see FEATURES.md "Archived features".) */
static bool is_protected_asset(const char *name)
{
    return strncmp(name, "rec_", 4) != 0;
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

/* `prefer_sd` false forces internal flash regardless of whether a card is
 * mounted -- see storage_write_begin_local() in storage.h for why (clips/
 * prompts need to survive the card being pulled mid-use; only real
 * recordings should follow it). */
static esp_err_t write_begin_ex(const char *name, size_t expected_len, bool prefer_sd,
                                 storage_writer_t *w)
{
    w->file = NULL;
    w->path[0] = '\0';
    bool sd = prefer_sd && sdcard_mounted();

    if (sd) {
        /* No eviction on SD -- see storage.h. A card is big enough that
         * silently deleting old recordings to make room would be a bad
         * surprise; fail instead once it's genuinely out of free space. */
        uint64_t total = 0, free_b = 0;
        if (sdcard_capacity(&total, &free_b) == ESP_OK && (uint64_t)expected_len > free_b) {
            return ESP_ERR_NO_MEM;
        }
        sd_full_path(w->path, sizeof(w->path), name);
    } else {
        if (expected_len > (size_t)CONFIG_STORAGE_RECORDING_CAP_BYTES) {
            return ESP_ERR_INVALID_SIZE;
        }
        full_path(w->path, sizeof(w->path), name);

        /* A forced-local write (prefer_sd=false) can leave a stale SD-side
         * copy of `name` around from before this split existed, or from a
         * name reused while a card was primary -- resolve_existing_path()'s
         * SD-first read order would keep resolving to that stale copy and
         * shadow this fresh internal write. A real SD-preferred write never
         * reaches this branch while a card is mounted, so this is a no-op
         * for recorder.c's callers. */
        if (!prefer_sd && sdcard_mounted()) {
            char sd_path[sizeof(w->path)];
            sd_full_path(sd_path, sizeof(sd_path), name);
            struct stat sdst;
            if (stat(sd_path, &sdst) == 0) unlink(sd_path);
        }
    }

    /* Overwriting an existing file of the same name: remove it first so its
     * old size isn't double-counted against the cap. */
    struct stat st;
    if (stat(w->path, &st) == 0) unlink(w->path);

    if (!sd) {
        size_t used = storage_used_bytes_internal();
        int guard = 64; /* bounded -- never loop forever if the dir is somehow stuck */
        while (used + expected_len > (size_t)CONFIG_STORAGE_RECORDING_CAP_BYTES && guard-- > 0) {
            size_t freed = evict_oldest();
            if (freed == 0) break; /* nothing left to evict */
            used -= freed;
        }
        if (used + expected_len > (size_t)CONFIG_STORAGE_RECORDING_CAP_BYTES) {
            return ESP_ERR_NO_MEM;
        }
    }

    FILE *f = fopen(w->path, "wb");
    if (!f) return ESP_FAIL;
    w->file = f;
    return ESP_OK;
}

esp_err_t storage_write_begin(const char *name, size_t expected_len, storage_writer_t *w)
{
    return write_begin_ex(name, expected_len, true, w);
}

esp_err_t storage_write_begin_local(const char *name, size_t expected_len, storage_writer_t *w)
{
    return write_begin_ex(name, expected_len, false, w);
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
    FILE *f = NULL;
    if (resolve_existing_path(name, path, sizeof(path))) {
        f = fopen(path, "rb");
    } else if (s_bundled_mounted) {
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
    /* Writable storage first (primary backend, then the other one -- see
     * resolve_existing_path()), so an upload/recording of the same name
     * overrides a bundled default without a rebuild. */
    char path[288];
    FILE *f = NULL;
    if (resolve_existing_path(name, path, sizeof(path))) {
        f = fopen(path, "rb");
    } else if (s_bundled_mounted) {
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
    if (!resolve_existing_path(name, path, sizeof(path))) {
        ESP_LOGW(TAG, "delete '%s': not found on any backend", name);
        return ESP_ERR_NOT_FOUND;
    }
    if (unlink(path) == 0) return ESP_OK;
    /* Log the real errno -- collapsing every unlink() failure into
     * ESP_ERR_NOT_FOUND (the old behavior) hid whatever the actual SPIFFS/
     * VFS error was, which made this impossible to diagnose from the webui
     * alone (it only ever reports the same generic "not found" either way). */
    ESP_LOGW(TAG, "delete '%s' (%s): unlink failed, errno=%d (%s)",
             name, path, errno, strerror(errno));
    return ESP_FAIL;
}

esp_err_t storage_patch(const char *name, size_t offset, const void *data, size_t len)
{
    char path[288];
    if (!resolve_existing_path(name, path, sizeof(path))) return ESP_ERR_NOT_FOUND;
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

static size_t list_dir(const char *mount, storage_entry_t *out, size_t max_entries)
{
    DIR *d = opendir(mount);
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
        snprintf(path, sizeof(path), "%s/%s", mount, de->d_name);
        if (stat(path, &st) != 0) continue;
        strncpy(names[n], de->d_name, STORAGE_MAX_NAME - 1);
        names[n][STORAGE_MAX_NAME - 1] = 0;
        stats[n] = st;
        n++;
    }
    closedir(d);

    /* insertion sort by mtime, newest first (Storage/Recordings pages both
     * read this -- newest-at-top is the more useful default for either:
     * a fresh 211 message or a just-uploaded clip is what you're looking
     * for right after it lands, not scrolled below everything older),
     * keep names[]/stats[] in lockstep */
    for (size_t i = 1; i < n; i++) {
        struct stat key_st = stats[i];
        char key_name[STORAGE_MAX_NAME];
        strncpy(key_name, names[i], STORAGE_MAX_NAME);
        int j = (int)i - 1;
        while (j >= 0 && cmp_mtime(&stats[j], &key_st) < 0) {
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

size_t storage_list_internal(storage_entry_t *out, size_t max_entries)
{
    return list_dir(MOUNT_POINT, out, max_entries);
}

size_t storage_list_sd(storage_entry_t *out, size_t max_entries)
{
    if (!sdcard_mounted()) return 0;
    return list_dir(SD_MOUNT_POINT, out, max_entries);
}

size_t storage_list(storage_entry_t *out, size_t max_entries)
{
    return sdcard_mounted() ? storage_list_sd(out, max_entries) : storage_list_internal(out, max_entries);
}

size_t storage_list_bundled(storage_entry_t *out, size_t max_entries)
{
    if (!s_bundled_mounted) return 0;
    return list_dir(BUNDLED_MOUNT_POINT, out, max_entries);
}
