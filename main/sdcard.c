/*
 * SD card driver — thin wrapper around the Waveshare BSP.
 *
 * The BSP handles slot selection (Slot 0, IO MUX pins 39-44) and LDO power.
 * Our custom sdmmc_host init was conflicting with esp_hosted which also uses
 * SDMMC Slot 1 (pins 14-19) for the C6 WiFi coprocessor.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "sdcard.h"
#include "bsp/esp32_p4_platform.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"

extern sdmmc_card_t *bsp_sdcard;

static const char *TAG = "sdcard";

/* Probe whether the card actually accepts writes; logs the failure reason
 * so we can distinguish "card locked" from "FS read-only" from "no card". */
static void sdcard_write_probe(void)
{
    const char *probe_path = SDCARD_ROOT "/.__rwtest";
    FILE *f = fopen(probe_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "write-probe FAILED: fopen(%s) errno=%d (%s)",
                 probe_path, errno, strerror(errno));
        if (bsp_sdcard) {
            ESP_LOGE(TAG, "card info: csd.read_bl_len=%d, csd.capacity=%llu sectors, "
                          "is_mmc=%d, is_sdio=%d",
                     bsp_sdcard->csd.read_block_len,
                     (unsigned long long)bsp_sdcard->csd.capacity,
                     bsp_sdcard->is_mmc, bsp_sdcard->is_sdio);
            sdmmc_card_print_info(stdout, bsp_sdcard);
        }
        return;
    }
    size_t w = fwrite("ok", 1, 2, f);
    fclose(f);
    if (w != 2) {
        ESP_LOGE(TAG, "write-probe partial write %zu/2 errno=%d", w, errno);
    } else {
        ESP_LOGI(TAG, "write-probe OK (%s writable)", SDCARD_ROOT);
        remove(probe_path);
    }

    /* Probe 2 — mkdir at root, then a write inside the new dir.
     * This isolates "FATFS mkdir is broken" from "writes fail in subdirs". */
    const char *test_dir  = SDCARD_ROOT "/.__rwdir";
    const char *test_file = SDCARD_ROOT "/.__rwdir/x";
    errno = 0;
    int mr = mkdir(test_dir, 0755);
    if (mr != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir-probe FAILED: mkdir(%s) ret=%d errno=%d (%s)",
                 test_dir, mr, errno, strerror(errno));
    } else {
        ESP_LOGI(TAG, "mkdir-probe OK (%s created or existed)", test_dir);
        FILE *g = fopen(test_file, "w");
        if (!g) {
            ESP_LOGE(TAG, "subdir-write-probe FAILED: fopen(%s) errno=%d (%s)",
                     test_file, errno, strerror(errno));
        } else {
            fwrite("ok", 1, 2, g);
            fclose(g);
            ESP_LOGI(TAG, "subdir-write-probe OK");
            remove(test_file);
        }
        rmdir(test_dir);
    }

    /* Probe 3 — list existing dirs at /sdcard root so we see what's there. */
    DIR *d = opendir(SDCARD_ROOT);
    if (d) {
        struct dirent *ent;
        int n = 0;
        while ((ent = readdir(d)) != NULL && n < 20) {
            ESP_LOGI(TAG, "  /sdcard entry: %s (type=%d)", ent->d_name, ent->d_type);
            n++;
        }
        closedir(d);
    } else {
        ESP_LOGE(TAG, "opendir(%s) failed errno=%d", SDCARD_ROOT, errno);
    }
}

esp_err_t sdcard_init(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "mounted at %s", SDCARD_ROOT);
    sdcard_write_probe();
    return ESP_OK;
}

void sdcard_deinit(void)
{
    bsp_sdcard_unmount();
    ESP_LOGI(TAG, "unmounted");
}
