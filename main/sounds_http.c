/*
 * sounds_http.c — device-side HTTP handlers for SD-card sound management,
 *                 plus boot-time SD integrity check.
 *
 * HTTP endpoints:
 *   GET  /sounds/manifest       — returns current .manifest.json (or empty manifest)
 *   PUT  /sounds/upload?path=X  — streams body directly to /sdcard/sounds/<X> in 4 KB chunks
 *   POST /sounds/commit         — atomically replaces .manifest.json with the request body
 *
 * Boot check:
 *   sounds_check_sd()  — compares embedded firmware manifest vs SD manifest, logs diffs
 *
 * Design constraints:
 *   - Never buffers a whole audio file in RAM.
 *   - All SD paths jail-checked against SDCARD_ROOT; no ".." traversal.
 *   - JSON parsed with a minimal hand-rolled scanner (no heap, no library).
 */

#include "sounds_http.h"
#include "sdcard.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"

static const char *TAG = "sounds_http";

#define SOUNDS_ROOT     SDCARD_ROOT "/sounds"
#define MANIFEST_PATH   SOUNDS_ROOT "/.manifest.json"
#define MANIFEST_TMP    SOUNDS_ROOT "/.manifest.tmp"
#define CHUNK_SIZE      4096
#define MAX_PATH_PARAM  256

/* ── Embedded manifest (baked in at build time by gen_manifest.py) ───────────── */
/* IDF EMBED_TXTFILES exposes these linker symbols. */
extern const char   sounds_manifest_json_start[] asm("_binary_sounds_manifest_json_start");
extern const char   sounds_manifest_json_end[]   asm("_binary_sounds_manifest_json_end");

/* ── Minimal flat-JSON scanner ───────────────────────────────────────────────── */
/*
 * The manifest is a single-level object:
 *   {"version":1,"files":{"rel/path.wav":"sha256hex", ...}}
 *
 * We only need to iterate the "files" sub-object extracting key/value pairs.
 * No recursive descent, no heap. Works on both the embedded blob and the
 * SD file (read in CHUNK_SIZE pieces, with a small overlap for split tokens).
 *
 * json_find_files_object(): advances *p past the opening '{' of the "files"
 * value. Returns true on success.
 *
 * json_next_kv(): reads the next "key":"value" pair from *p, writing into
 * key_buf/val_buf (each MAX_PATH_PARAM bytes). Advances *p past the pair.
 * Returns true while more pairs exist; false at '}' or end of string.
 */

static bool json_skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
    return **p != '\0';
}

/* Read a JSON string starting at *p (which must point at opening '"').
 * Writes unescaped content to buf (max len-1 chars + NUL). Advances *p past
 * the closing '"'. Returns false on malformed input. */
static bool json_read_string(const char **p, char *buf, size_t len)
{
    if (**p != '"') return false;
    (*p)++;
    size_t i = 0;
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            if (!**p) return false;
        }
        if (i + 1 < len) buf[i++] = **p;
        (*p)++;
    }
    if (**p != '"') return false;
    (*p)++;
    buf[i] = '\0';
    return true;
}

/* Advance *p to the opening '{' of the "files" sub-object. */
static bool json_find_files_object(const char **p)
{
    /* Look for the literal sequence "files" : { */
    const char *s = strstr(*p, "\"files\"");
    if (!s) return false;
    s += 7; /* past "files" */
    json_skip_ws(&s);
    if (*s != ':') return false;
    s++;
    json_skip_ws(&s);
    if (*s != '{') return false;
    s++;
    *p = s;
    return true;
}

/* Read next "key":"value" pair from within the files object.
 * *p should be just inside the '{', or just past the previous ','.
 * Returns true and fills key/val on success; false at end of object. */
static bool json_next_kv(const char **p,
                          char *key, size_t key_len,
                          char *val, size_t val_len)
{
    json_skip_ws(p);
    if (**p == '}' || **p == '\0') return false;
    if (**p == ',') { (*p)++; json_skip_ws(p); }
    if (**p == '}' || **p == '\0') return false;

    if (!json_read_string(p, key, key_len)) return false;
    json_skip_ws(p);
    if (**p != ':') return false;
    (*p)++;
    json_skip_ws(p);
    if (!json_read_string(p, val, val_len)) return false;
    return true;
}

/* ── Path safety ─────────────────────────────────────────────────────────────── */

static bool path_is_safe(const char *rel)
{
    if (!rel || rel[0] == '\0') return false;
    if (rel[0] == '/')          return false;
    if (strstr(rel, ".."))      return false;
    return strnlen(rel, MAX_PATH_PARAM) < MAX_PATH_PARAM;
}

static bool build_sd_path(char *dst, size_t dst_size, const char *rel)
{
    int n = snprintf(dst, dst_size, "%s/%s", SOUNDS_ROOT, rel);
    return (n > 0 && (size_t)n < dst_size);
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* ── Boot-time SD check ──────────────────────────────────────────────────────── */

/*
 * Read the entire SD manifest JSON into a heap buffer.
 * Returns malloc'd pointer (caller must free) or NULL on error.
 * *out_len receives the length (excluding NUL).
 */
static char *read_sd_manifest(size_t *out_len)
{
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 2 * 1024 * 1024) { /* sanity: max 2 MB manifest */
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

int sounds_check_sd(void)
{
    int missing = 0, changed = 0, ok = 0;

    /* Embedded manifest (zero-copy: direct pointer into flash) */
    const char *emb = sounds_manifest_json_start;
    if (!json_find_files_object(&emb)) {
        ESP_LOGW(TAG, "boot check: embedded manifest malformed — skipping SD check");
        return 0;
    }

    /* SD manifest — read into heap so we can do O(1) substring lookups */
    size_t sd_len = 0;
    char  *sd_buf = read_sd_manifest(&sd_len);
    if (!sd_buf) {
        ESP_LOGW(TAG, "boot check: no SD manifest at %s — all files need upload", MANIFEST_PATH);
        /* Count how many files the embedded manifest expects */
        const char *scan = emb;
        char k[MAX_PATH_PARAM], v[MAX_PATH_PARAM];
        while (json_next_kv(&scan, k, sizeof(k), v, sizeof(v))) missing++;
        ESP_LOGW(TAG, "boot check: %d file(s) missing from SD", missing);
        return missing;
    }

    /* Walk every entry in the embedded manifest */
    char key[MAX_PATH_PARAM], emb_hash[128];
    while (json_next_kv(&emb, key, sizeof(key), emb_hash, sizeof(emb_hash))) {
        /* Look up this key in the SD manifest JSON by substring search.
         * This is O(n*m) but runs once at boot on a 200 KB string — fast enough. */
        bool found = false;
        const char *sd_scan = sd_buf;
        const char *sd_files = sd_buf;
        if (json_find_files_object(&sd_files)) sd_scan = sd_files;

        char sd_key[MAX_PATH_PARAM], sd_hash[128];
        const char *cur = sd_scan;
        while (json_next_kv(&cur, sd_key, sizeof(sd_key), sd_hash, sizeof(sd_hash))) {
            if (strcmp(sd_key, key) == 0) {
                found = true;
                if (strcmp(sd_hash, emb_hash) != 0) {
                    ESP_LOGW(TAG, "changed: %s", key);
                    changed++;
                } else {
                    ok++;
                }
                break;
            }
        }
        if (!found) {
            ESP_LOGW(TAG, "missing: %s", key);
            missing++;
        }
    }

    free(sd_buf);

    ESP_LOGI(TAG, "SD check: %d ok, %d missing, %d changed", ok, missing, changed);
    return missing + changed;
}

/* ── GET /sounds/manifest ────────────────────────────────────────────────────── */

static esp_err_t manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) {
        const char *empty = "{\"version\":1,\"files\":{}}";
        httpd_resp_sendstr(req, empty);
        return ESP_OK;
    }

    char *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n;
    while ((n = fread(buf, 1, CHUNK_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            free(buf);
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── PUT /sounds/upload?path=<relative-wav-path> ─────────────────────────────── */

static esp_err_t upload_put_handler(httpd_req_t *req)
{
    char rel[MAX_PATH_PARAM] = "";
    if (httpd_req_get_url_query_len(req) > 0) {
        char qs[MAX_PATH_PARAM * 2];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
            httpd_query_key_value(qs, "path", rel, sizeof(rel));
    }

    if (!path_is_safe(rel)) {
        ESP_LOGW(TAG, "upload: bad path '%s'", rel);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_OK;
    }

    char abs_path[512];
    if (!build_sd_path(abs_path, sizeof(abs_path), rel)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        return ESP_OK;
    }

    char *slash = strrchr(abs_path, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(abs_path);
        mkdir(abs_path, 0755);
        *slash = '/';
    }

    /* Skip-if-same-size: drain the request body but don't touch the SD card
     * if a file already exists at this path with matching byte length.
     * This makes the client's pre-flight /sd/stat check redundant-safe and
     * lets clients that skip the check still benefit. */
    struct stat existing_st;
    if (req->content_len > 0 &&
        stat(abs_path, &existing_st) == 0 && !S_ISDIR(existing_st.st_mode) &&
        existing_st.st_size == (off_t)req->content_len) {
        /* Drain body so the connection stays usable, then 200 with "skipped". */
        char drain[1024];
        int remaining_drain = req->content_len;
        while (remaining_drain > 0) {
            int to_read = (remaining_drain < (int)sizeof(drain)) ? remaining_drain : (int)sizeof(drain);
            int n = httpd_req_recv(req, drain, to_read);
            if (n <= 0) break;
            remaining_drain -= n;
        }
        ESP_LOGI(TAG, "upload: skipped %s (already %d bytes)", rel, req->content_len);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "X-Upload-Skipped", "1");
        httpd_resp_sendstr(req, "skipped");
        return ESP_OK;
    }

    FILE *f = fopen(abs_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload: cannot open %s: %s", abs_path, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open file");
        return ESP_OK;
    }

    char *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    int remaining = req->content_len;
    int total_written = 0;
    bool ok = true;
    const char *fail_reason = NULL;
    int fail_errno = 0;
    while (remaining > 0) {
        int to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            ok = false;
            fail_reason = "recv";
            fail_errno = errno;
            ESP_LOGE(TAG, "upload: recv failed for %s after %d/%d bytes (ret=%d errno=%d %s)",
                     rel, total_written, req->content_len, received, errno, strerror(errno));
            break;
        }
        size_t w = fwrite(buf, 1, received, f);
        if (w != (size_t)received) {
            ok = false;
            fail_reason = "fwrite";
            fail_errno = errno;
            ESP_LOGE(TAG, "upload: fwrite short for %s after %d/%d bytes (wrote %zu/%d errno=%d %s)",
                     rel, total_written, req->content_len, w, received, errno, strerror(errno));
            break;
        }
        total_written += received;
        remaining -= received;
    }
    int fflush_rc = 0;
    int fsync_errno = 0;
    if (ok) {
        fflush_rc = fflush(f);
        if (fflush_rc != 0) {
            fsync_errno = errno;
            ESP_LOGE(TAG, "upload: fflush failed for %s (errno=%d %s)", rel, errno, strerror(errno));
            ok = false;
            fail_reason = "fflush";
            fail_errno = fsync_errno;
        }
    }
    free(buf);
    if (fclose(f) != 0 && ok) {
        ESP_LOGE(TAG, "upload: fclose failed for %s (errno=%d %s)", rel, errno, strerror(errno));
        ok = false;
        fail_reason = "fclose";
        fail_errno = errno;
    }

    if (!ok) {
        remove(abs_path);
        char msg[96];
        snprintf(msg, sizeof(msg), "write failed (%s errno=%d %s)",
                 fail_reason ? fail_reason : "?", fail_errno, strerror(fail_errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "upload: saved %s (%d bytes)", rel, req->content_len);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── POST /sounds/commit ─────────────────────────────────────────────────────── */

static esp_err_t commit_post_handler(httpd_req_t *req)
{
    FILE *f = fopen(MANIFEST_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "commit: cannot open tmp %s: %s", MANIFEST_TMP, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write manifest");
        return ESP_OK;
    }

    char *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    int remaining = req->content_len;
    int total_written = 0;
    bool ok = true;
    const char *fail_reason = NULL;
    int fail_errno = 0;
    while (remaining > 0) {
        int to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            ok = false; fail_reason = "recv"; fail_errno = errno;
            ESP_LOGE(TAG, "commit: recv failed after %d/%d (ret=%d errno=%d %s)",
                     total_written, req->content_len, received, errno, strerror(errno));
            break;
        }
        size_t w = fwrite(buf, 1, received, f);
        if (w != (size_t)received) {
            ok = false; fail_reason = "fwrite"; fail_errno = errno;
            ESP_LOGE(TAG, "commit: fwrite short after %d/%d (wrote %zu/%d errno=%d %s)",
                     total_written, req->content_len, w, received, errno, strerror(errno));
            break;
        }
        total_written += received;
        remaining -= received;
    }
    if (ok && fflush(f) != 0) {
        ok = false; fail_reason = "fflush"; fail_errno = errno;
        ESP_LOGE(TAG, "commit: fflush failed (errno=%d %s)", errno, strerror(errno));
    }
    free(buf);
    if (fclose(f) != 0 && ok) {
        ok = false; fail_reason = "fclose"; fail_errno = errno;
        ESP_LOGE(TAG, "commit: fclose failed (errno=%d %s)", errno, strerror(errno));
    }

    if (!ok) {
        remove(MANIFEST_TMP);
        char msg[96];
        snprintf(msg, sizeof(msg), "write failed (%s errno=%d %s)",
                 fail_reason ? fail_reason : "?", fail_errno, strerror(fail_errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_OK;
    }

    if (remove(MANIFEST_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "commit: remove old manifest failed (errno=%d %s)", errno, strerror(errno));
    }
    if (rename(MANIFEST_TMP, MANIFEST_PATH) != 0) {
        ESP_LOGE(TAG, "commit: rename %s -> %s failed: %s",
                 MANIFEST_TMP, MANIFEST_PATH, strerror(errno));
        char msg[96];
        snprintf(msg, sizeof(msg), "rename failed (errno=%d %s)", errno, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "manifest committed (%d bytes)", req->content_len);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── GET /sd/ls?path=<relative-dir> ─────────────────────────────────────────── */
/*
 * Lists immediate children of /sdcard/<path>. Returns JSON:
 *   {"dirs":["KitA","KitB"],"files":["kick.wav",...]}
 * Only one level deep; no recursion. Rejects unsafe paths.
 */

static esp_err_t sd_ls_get_handler(httpd_req_t *req)
{
    char rel[MAX_PATH_PARAM] = "";
    if (httpd_req_get_url_query_len(req) > 0) {
        char qs[MAX_PATH_PARAM * 2];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
            httpd_query_key_value(qs, "path", rel, sizeof(rel));
    }

    char abs_path[512];
    if (rel[0] == '\0') {
        snprintf(abs_path, sizeof(abs_path), "%s", SDCARD_ROOT);
    } else {
        if (!path_is_safe(rel)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
            return ESP_OK;
        }
        snprintf(abs_path, sizeof(abs_path), "%s/%s", SDCARD_ROOT, rel);
    }

    DIR *dir = opendir(abs_path);
    if (!dir) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"dirs\":[],\"files\":[]}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"dirs\":[", 9);

    struct dirent *entry;
    bool first_dir = true;
    /* Two-pass: first dirs, then files */
    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR) continue;
        char quoted[MAX_PATH_PARAM + 4];
        /* Escape backslashes and double-quotes in name */
        char esc[MAX_PATH_PARAM * 2];
        size_t si = 0, di = 0;
        const char *n = entry->d_name;
        while (n[si] && di + 2 < sizeof(esc)) {
            if (n[si] == '"' || n[si] == '\\') esc[di++] = '\\';
            esc[di++] = n[si++];
        }
        esc[di] = '\0';
        int len = snprintf(quoted, sizeof(quoted), "%s\"%s\"", first_dir ? "" : ",", esc);
        httpd_resp_send_chunk(req, quoted, len);
        first_dir = false;
    }

    httpd_resp_send_chunk(req, "],\"files\":[", 11);

    bool first_file = true;
    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_DIR) continue;
        char quoted[MAX_PATH_PARAM + 4];
        char esc[MAX_PATH_PARAM * 2];
        size_t si = 0, di = 0;
        const char *n = entry->d_name;
        while (n[si] && di + 2 < sizeof(esc)) {
            if (n[si] == '"' || n[si] == '\\') esc[di++] = '\\';
            esc[di++] = n[si++];
        }
        esc[di] = '\0';
        int len = snprintf(quoted, sizeof(quoted), "%s\"%s\"", first_file ? "" : ",", esc);
        httpd_resp_send_chunk(req, quoted, len);
        first_file = false;
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── POST /upload?path=<relative-dir> (multipart/form-data) ─────────────────── */
/*
 * Accepts multipart/form-data from a mobile browser <input type="file">.
 * Streams each file part directly to /sdcard/<path>/<filename> in 4 KB chunks.
 * Creates the target directory if missing. Accepts .wav files only.
 * Returns JSON {"ok":true,"saved":["file1.wav",...]}.
 *
 * Multipart parsing: we scan the raw body for boundary separators and
 * Content-Disposition headers — no external MIME library required.
 */

#define UPLOAD_BUF  4096
#define MAX_SAVED   32

/* Extract filename from a Content-Disposition header line.
 * Looks for filename="<name>" or filename*=UTF-8''<name>.
 * Writes into fname_buf (max fname_len). Returns true on success. */
static bool extract_filename(const char *header, char *fname_buf, size_t fname_len)
{
    const char *p = strstr(header, "filename=\"");
    if (p) {
        p += 10;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < fname_len)
            fname_buf[i++] = *p++;
        fname_buf[i] = '\0';
        return i > 0;
    }
    return false;
}

/* Return true if filename ends with .wav (case-insensitive) */
static bool is_wav_filename(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *ext = name + n - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'w' || ext[1] == 'W') &&
            (ext[2] == 'a' || ext[2] == 'A') &&
            (ext[3] == 'v' || ext[3] == 'V'));
}

static esp_err_t mobile_upload_post_handler(httpd_req_t *req)
{
    /* --- extract boundary from Content-Type header --- */
    char ct[256] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));

    const char *bnd_start = strstr(ct, "boundary=");
    if (!bnd_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing boundary");
        return ESP_OK;
    }
    bnd_start += 9;
    /* Remove optional quotes */
    if (*bnd_start == '"') bnd_start++;
    char boundary[128];
    size_t bi = 0;
    while (bnd_start[bi] && bnd_start[bi] != '"' && bi + 1 < sizeof(boundary)) {
        boundary[bi] = bnd_start[bi];
        bi++;
    }
    boundary[bi] = '\0';
    if (bi == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty boundary");
        return ESP_OK;
    }

    /* --- get target dir --- */
    char rel_dir[MAX_PATH_PARAM] = "sounds";
    if (httpd_req_get_url_query_len(req) > 0) {
        char qs[MAX_PATH_PARAM * 2];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
            httpd_query_key_value(qs, "path", rel_dir, sizeof(rel_dir));
    }
    if (!path_is_safe(rel_dir)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_OK;
    }
    char abs_dir[512];
    snprintf(abs_dir, sizeof(abs_dir), "%s/%s", SDCARD_ROOT, rel_dir);
    mkdir_p(abs_dir);
    mkdir(abs_dir, 0755);

    /* --- read entire body into a heap buffer (files are small WAVs from mobile) --- */
    int body_len = req->content_len;
    if (body_len <= 0 || body_len > 8 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or unknown size");
        return ESP_OK;
    }
    char *body = malloc((size_t)body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    int received = 0;
    while (received < body_len) {
        int chunk = httpd_req_recv(req, body + received, body_len - received);
        if (chunk <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv error"); return ESP_OK; }
        received += chunk;
    }
    body[received] = '\0';

    /* --- parse multipart parts --- */
    /* boundary delimiter: "--<boundary>" */
    char delim[140];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t delim_len = strlen(delim);

    char saved_names[MAX_SAVED][MAX_PATH_PARAM];
    int  saved_count = 0;

    const char *pos = body;
    const char *body_end = body + received;

    while (pos < body_end) {
        /* Find next boundary */
        const char *b = memmem(pos, (size_t)(body_end - pos), delim, delim_len);
        if (!b) break;
        b += delim_len;
        /* "--" after boundary = epilogue */
        if (b[0] == '-' && b[1] == '-') break;
        /* skip CRLF after boundary */
        if (b[0] == '\r') b++;
        if (b[0] == '\n') b++;

        /* Find end of headers (blank line "\r\n\r\n" or "\n\n") */
        const char *hdr_end = memmem(b, (size_t)(body_end - b), "\r\n\r\n", 4);
        const char *data_start;
        if (hdr_end) {
            data_start = hdr_end + 4;
        } else {
            hdr_end = memmem(b, (size_t)(body_end - b), "\n\n", 2);
            if (!hdr_end) { pos = b; continue; }
            data_start = hdr_end + 2;
        }

        /* Extract Content-Disposition from headers */
        char fname[MAX_PATH_PARAM] = "";
        /* Headers block: b .. hdr_end */
        char hdrs[512];
        size_t hlen = (size_t)(hdr_end - b);
        if (hlen >= sizeof(hdrs)) hlen = sizeof(hdrs) - 1;
        memcpy(hdrs, b, hlen);
        hdrs[hlen] = '\0';
        extract_filename(hdrs, fname, sizeof(fname));

        /* Find data end = next boundary */
        const char *data_end = memmem(data_start, (size_t)(body_end - data_start), delim, delim_len);
        if (!data_end) { pos = data_start; continue; }
        /* Strip trailing CRLF before boundary */
        if (data_end > data_start && data_end[-1] == '\n') data_end--;
        if (data_end > data_start && data_end[-1] == '\r') data_end--;
        size_t data_len = (size_t)(data_end - data_start);

        pos = data_end;

        if (fname[0] == '\0') continue;
        if (!is_wav_filename(fname)) {
            ESP_LOGW(TAG, "mobile upload: rejected non-WAV '%s'", fname);
            continue;
        }

        /* Sanitize filename: no slashes */
        for (char *c = fname; *c; c++) if (*c == '/' || *c == '\\') *c = '_';

        char abs_path[512 + MAX_PATH_PARAM + 1];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, fname);

        /* Skip-if-same-size: same rationale as the PUT handler. Multipart
         * gives us the part length up front (data_len), so we compare to the
         * existing file size on SD. Match → record as "saved" and move on. */
        struct stat existing_st;
        if (stat(abs_path, &existing_st) == 0 && !S_ISDIR(existing_st.st_mode) &&
            existing_st.st_size == (off_t)data_len) {
            ESP_LOGI(TAG, "mobile upload: skipped %s (already %zu bytes)", fname, data_len);
            if (saved_count < MAX_SAVED)
                strncpy(saved_names[saved_count++], fname, MAX_PATH_PARAM - 1);
            continue;
        }

        FILE *f = fopen(abs_path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "mobile upload: cannot open %s: %s", abs_path, strerror(errno));
            continue;
        }
        bool write_ok = (fwrite(data_start, 1, data_len, f) == data_len);
        fclose(f);

        if (!write_ok) {
            remove(abs_path);
            ESP_LOGE(TAG, "mobile upload: write failed for %s", fname);
            continue;
        }

        ESP_LOGI(TAG, "mobile upload: saved %s (%zu bytes)", fname, data_len);
        if (saved_count < MAX_SAVED)
            strncpy(saved_names[saved_count++], fname, MAX_PATH_PARAM - 1);
    }
    free(body);

    /* Build JSON response */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"ok\":true,\"saved\":[", 20);
    for (int i = 0; i < saved_count; i++) {
        char quoted[MAX_PATH_PARAM + 4];
        int len = snprintf(quoted, sizeof(quoted), "%s\"%s\"", i ? "," : "", saved_names[i]);
        httpd_resp_send_chunk(req, quoted, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /sd?path=<relative-path> ────────────────────────────────────────────── */
/*
 * Streams a single file from /sdcard/<path> to the browser.
 * Used by the sample cutter to fetch WAV files for waveform rendering.
 * Path must be relative to /sdcard/ and must not contain "..".
 * Streams in 4 KB chunks; no full-file buffering.
 */
#define SD_DL_CHUNK 4096

static esp_err_t sd_get_handler(httpd_req_t *req)
{
    char rel[MAX_PATH_PARAM] = "";
    if (httpd_req_get_url_query_len(req) > 0) {
        char qs[MAX_PATH_PARAM * 2];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
            httpd_query_key_value(qs, "path", rel, sizeof(rel));
    }
    if (!path_is_safe(rel)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    char abs_path[MAX_PATH_PARAM + 32];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", SDCARD_ROOT, rel);

    struct stat st;
    if (stat(abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_OK;
    }

    /* Sniff content type from extension */
    const char *ext = strrchr(rel, '.');
    if (ext && strcasecmp(ext, ".wav") == 0)
        httpd_resp_set_type(req, "audio/wav");
    else
        httpd_resp_set_type(req, "application/octet-stream");

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    static char chunk[SD_DL_CHUNK];
    size_t nread;
    while ((nread = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)nread) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /sd/stat?path=<sd-root-relative-path> ────────────────────────────────
 * Lightweight existence/size check used by clients to skip re-uploading files
 * that already match on the SD card. The `path` query parameter is resolved
 * relative to SDCARD_ROOT, matching the convention used by /sd and /sd/ls.
 * (Clients targeting /sounds/upload should prefix with "sounds/" — that PUT
 *  endpoint is rooted at SDCARD_ROOT/sounds.)
 * Always returns 200 with a small JSON body:
 *   {"exists": true,  "size": N}    — file present
 *   {"exists": false}                — missing or is a directory
 */
static esp_err_t sd_stat_get_handler(httpd_req_t *req)
{
    char rel[MAX_PATH_PARAM] = "";
    if (httpd_req_get_url_query_len(req) > 0) {
        char qs[MAX_PATH_PARAM * 2];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
            httpd_query_key_value(qs, "path", rel, sizeof(rel));
    }
    if (!path_is_safe(rel)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    char abs_path[512];
    int pn = snprintf(abs_path, sizeof(abs_path), "%s/%s", SDCARD_ROOT, rel);
    if (pn <= 0 || (size_t)pn >= sizeof(abs_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    struct stat st;
    if (stat(abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_sendstr(req, "{\"exists\":false}");
        return ESP_OK;
    }

    char body[64];
    int n = snprintf(body, sizeof(body),
                     "{\"exists\":true,\"size\":%lld}",
                     (long long)st.st_size);
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

/* ── GET /sd/ls/recursive?path=<sd-root-relative-dir> ─────────────────────────
 * Streams an NDJSON listing of every regular file under the given directory:
 *   {"path":"sub/dir/foo.wav","size":12345}
 *   {"path":"sub/dir/bar.wav","size":67890}
 *   ...
 * One NDJSON object per line. Paths are relative to the requested root (no
 * leading slash, forward slashes only). Skips hidden entries (dot-prefixed).
 * Used by clients to plan delta uploads in a single roundtrip instead of one
 * /sd/stat call per file. Depth-limited to keep stack/heap bounded.
 */
#define SD_LS_RECURSIVE_MAX_DEPTH 12

static esp_err_t sd_ls_recursive_get_handler(httpd_req_t *req)
{
    char rel[MAX_PATH_PARAM] = "";
    if (httpd_req_get_url_query_len(req) > 0) {
        char qs[MAX_PATH_PARAM * 2];
        if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
            httpd_query_key_value(qs, "path", rel, sizeof(rel));
    }

    char root_abs[512];
    if (rel[0] == '\0') {
        snprintf(root_abs, sizeof(root_abs), "%s", SDCARD_ROOT);
    } else {
        if (!path_is_safe(rel)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
            return ESP_OK;
        }
        int pn = snprintf(root_abs, sizeof(root_abs), "%s/%s", SDCARD_ROOT, rel);
        if (pn <= 0 || (size_t)pn >= sizeof(root_abs)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
            return ESP_OK;
        }
    }

    httpd_resp_set_type(req, "application/x-ndjson");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Iterative DFS so we don't blow the task stack on deep trees. */
    typedef struct {
        DIR    *dir;
        size_t  prefix_len;   /* length of cur_rel including trailing '/' or 0 at root */
    } frame_t;
    frame_t stack[SD_LS_RECURSIVE_MAX_DEPTH];
    int sp = 0;

    DIR *root_dir = opendir(root_abs);
    if (!root_dir) {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }
    stack[sp++] = (frame_t){ .dir = root_dir, .prefix_len = 0 };

    /* Buffers reused across the whole walk. cur_rel accumulates the path
     * relative to the request root; cur_abs is "{root_abs}/{cur_rel}". */
    char cur_rel[MAX_PATH_PARAM];
    char cur_abs[512];
    cur_rel[0] = '\0';

    while (sp > 0) {
        frame_t *fr = &stack[sp - 1];
        struct dirent *entry = readdir(fr->dir);
        if (!entry) {
            closedir(fr->dir);
            sp--;
            /* Pop one path segment off cur_rel so siblings continue cleanly. */
            if (sp > 0) cur_rel[stack[sp - 1].prefix_len] = '\0';
            else cur_rel[0] = '\0';
            continue;
        }
        if (entry->d_name[0] == '.') continue;

        /* Append entry->d_name to cur_rel at fr->prefix_len. */
        size_t name_len = strlen(entry->d_name);
        if (fr->prefix_len + name_len + 1 >= sizeof(cur_rel)) continue;
        memcpy(cur_rel + fr->prefix_len, entry->d_name, name_len + 1);

        /* Compose absolute path for stat / opendir. */
        int an = snprintf(cur_abs, sizeof(cur_abs), "%s/%s", root_abs, cur_rel);
        if (an <= 0 || (size_t)an >= sizeof(cur_abs)) {
            cur_rel[fr->prefix_len] = '\0';
            continue;
        }

        struct stat st;
        if (stat(cur_abs, &st) != 0) {
            cur_rel[fr->prefix_len] = '\0';
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (sp >= SD_LS_RECURSIVE_MAX_DEPTH) {
                cur_rel[fr->prefix_len] = '\0';
                continue;
            }
            DIR *sub = opendir(cur_abs);
            if (!sub) {
                cur_rel[fr->prefix_len] = '\0';
                continue;
            }
            size_t new_prefix = fr->prefix_len + name_len;
            if (new_prefix + 1 >= sizeof(cur_rel)) {
                closedir(sub);
                cur_rel[fr->prefix_len] = '\0';
                continue;
            }
            cur_rel[new_prefix]     = '/';
            cur_rel[new_prefix + 1] = '\0';
            stack[sp++] = (frame_t){ .dir = sub, .prefix_len = new_prefix + 1 };
            /* Leave cur_rel ending in '/' for the next iteration's prefix. */
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            /* Escape backslashes and double-quotes in cur_rel for JSON. */
            char esc[MAX_PATH_PARAM * 2];
            size_t si = 0, di = 0;
            while (cur_rel[si] && di + 2 < sizeof(esc)) {
                if (cur_rel[si] == '"' || cur_rel[si] == '\\') esc[di++] = '\\';
                esc[di++] = cur_rel[si++];
            }
            esc[di] = '\0';

            char line[MAX_PATH_PARAM * 2 + 64];
            int ln = snprintf(line, sizeof(line),
                              "{\"path\":\"%s\",\"size\":%lld}\n",
                              esc, (long long)st.st_size);
            if (ln > 0 && (size_t)ln < sizeof(line)) {
                if (httpd_resp_send_chunk(req, line, ln) != ESP_OK) {
                    /* Client disconnected — clean up open dirs and bail. */
                    while (sp > 0) closedir(stack[--sp].dir);
                    return ESP_OK;
                }
            }
        }
        /* Restore cur_rel for the next sibling. */
        cur_rel[fr->prefix_len] = '\0';
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /sd/df ───────────────────────────────────────────────────────────────
 * Returns FATFS's view of the mounted SD card capacity as JSON:
 *   {"total":N,"used":N,"free":N}
 * Sizes are in bytes. This is the device's authoritative number — use it
 * instead of host-side df, which can disagree if the card has multiple
 * partitions or was formatted exFAT (FATFS won't mount the exFAT volume).
 */
static esp_err_t sd_df_get_handler(httpd_req_t *req)
{
    uint64_t total = 0, free_ = 0;
    esp_err_t err = esp_vfs_fat_info(SDCARD_ROOT, &total, &free_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd_df: esp_vfs_fat_info(%s) err=0x%x (%s)",
                 SDCARD_ROOT, err, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "fat_info failed");
        return ESP_OK;
    }

    uint64_t used = (total > free_) ? (total - free_) : 0;
    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"total\":%llu,\"used\":%llu,\"free\":%llu}",
                     (unsigned long long)total,
                     (unsigned long long)used,
                     (unsigned long long)free_);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

/* ── Registration ────────────────────────────────────────────────────────────── */

void sounds_http_register(httpd_handle_t server)
{
    mkdir(SOUNDS_ROOT, 0755);

    static const httpd_uri_t manifest_uri = {
        .uri = "/sounds/manifest", .method = HTTP_GET,  .handler = manifest_get_handler };
    static const httpd_uri_t upload_uri = {
        .uri = "/sounds/upload",   .method = HTTP_PUT,  .handler = upload_put_handler  };
    static const httpd_uri_t commit_uri = {
        .uri = "/sounds/commit",   .method = HTTP_POST, .handler = commit_post_handler  };
    static const httpd_uri_t ls_uri = {
        .uri = "/sd/ls",           .method = HTTP_GET,  .handler = sd_ls_get_handler   };
    static const httpd_uri_t mob_upload_uri = {
        .uri = "/upload",          .method = HTTP_POST, .handler = mobile_upload_post_handler };
    static const httpd_uri_t sd_get_uri = {
        .uri = "/sd",              .method = HTTP_GET,  .handler = sd_get_handler };
    static const httpd_uri_t sd_stat_uri = {
        .uri = "/sd/stat",         .method = HTTP_GET,  .handler = sd_stat_get_handler };
    static const httpd_uri_t sd_ls_rec_uri = {
        .uri = "/sd/ls/recursive", .method = HTTP_GET,  .handler = sd_ls_recursive_get_handler };
    static const httpd_uri_t sd_df_uri = {
        .uri = "/sd/df",           .method = HTTP_GET,  .handler = sd_df_get_handler };

    httpd_register_uri_handler(server, &manifest_uri);
    httpd_register_uri_handler(server, &upload_uri);
    httpd_register_uri_handler(server, &commit_uri);
    httpd_register_uri_handler(server, &mob_upload_uri);
    /* More specific paths registered before less specific so exact-match wins
     * on routers that walk handlers in registration order. */
    httpd_register_uri_handler(server, &sd_ls_rec_uri);
    httpd_register_uri_handler(server, &sd_df_uri);
    httpd_register_uri_handler(server, &ls_uri);
    httpd_register_uri_handler(server, &sd_stat_uri);
    httpd_register_uri_handler(server, &sd_get_uri);

    ESP_LOGI(TAG, "registered /sounds/{manifest,upload,commit}, /sd/{ls,ls/recursive,stat,df}, /sd, /upload");
}
