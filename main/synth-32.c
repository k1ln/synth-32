/*
 * synth-32.c — entry point only.
 * All subsystems live in their own modules:
 *   gfx.c        RGB565 framebuffer + LCD init
 *   audio.c      I2S / ES8311 codec + wavetables + voice pool + ADSR
 *   wifi_http.c  SoftAP + NVS credentials + HTTP server
 *   ui.c         Touch, piano, sliders, tab router, ui_task
 *   wav_player.c Phase-1 SD-streaming WAV engine
 *   sdcard.c     SD-card mount/unmount
 *   sounds_http.c HTTP sound-upload handlers
 *   mdns_init.c  mDNS + captive-portal DNS responder
 */

#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "sdcard.h"
#include "sounds_http.h"
#include "mdns_init.h"
#include "wav_player.h"
#include "audio.h"
#include "lane.h"
#include "wifi_http.h"
#include "ui.h"
#include "settings.h"
#include "song.h"
#include "synth_voice.h"

static const char *TAG = "synth32";

/* Global song state in PSRAM — 64 lanes × ~1.2 KB each is too large for DRAM */
EXT_RAM_BSS_ATTR song_t g_song;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sdcard_init();
    sounds_check_sd();
    /* Pre-create song dir so the browser never lists into a missing dir */
    mkdir(SONG_DIR, 0755);

    /* Load device settings before any subsystem reads them */
    settings_load();

    /* ── Core 1: audio ───────────────────────────────────────────────────────
     * i2c_init   — bus recovery then bsp_i2c_init (needed by ES8311 + GT911)
     * audio_init — I2S + ES8311 codec open; spawns audio_task on core 1
     * wav_engine_init — PSRAM ring alloc + sd_stream_task on core 0
     *   (called after audio_init so the PSRAM heap is fully settled)
     * ───────────────────────────────────────────────────────────────────── */
    /* Wire up the WAV lane pool table before any song load so drum-row sample
     * preloads can claim slots. The sd_stream_task is still spawned later via
     * wav_engine_init() (deferred to avoid preempting this init sequence). */
    wav_pool_init();

    /* song_new() applies settings BPM/PPQN/bpb; auto-reload last song if set */
    song_new();
    if (g_settings.last_song[0] != '\0') {
        if (!song_load(g_settings.last_song))
            ESP_LOGW(TAG, "auto-reload failed: %s", g_settings.last_song);
    }

    i2c_init();
    wavetable_init();
    sv_wt_init();       /* fill sv_wt[] tables used by all Phase-3 synths */
    audio_init();       /* audio_task pinned to core 1, prio 20 */
    song_autosave_task_start(); /* time-based autosave on core 0, prio 3 */

    /* ── Core 0: UI + WiFi ───────────────────────────────────────────────── */
    xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 5, NULL, 0);

    /* wav_engine_init last: sd_stream_task (prio 10) would preempt main_task
       (prio 1) if created earlier, stalling the init sequence. By this point
       main_task is nearly done so the preemption is harmless. */
    wav_engine_init();  /* sd_stream_task pinned to core 0, prio 10 */

    /*
     * ESP32-P4 has no native WiFi. Traffic routes over SDIO to the companion
     * ESP32-C6 running esp_hosted slave firmware.  After esp_hosted_init() all
     * standard esp_wifi_* / esp_netif_* APIs work transparently.
     *
     * Prerequisites (sdkconfig.defaults + idf_component.yml):
     *   CONFIG_ESP_HOSTED_ENABLED=y
     *   CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y
     *   CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
     *   CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=4
     *   CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=20000
     *   CONFIG_ESP_WIFI_REMOTE_ENABLED=y
     *   CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y
     *   CONFIG_ESP_HOSTED_USE_MEMPOOL=n  (P4 v1.3 silicon workaround)
     */
    ESP_LOGI(TAG, "Initialising ESP-Hosted coprocessor (ESP32-C6 over SDIO)...");
    ret = esp_hosted_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %d — C6 not responding", ret);
        /* Non-fatal: touch/audio/SD still work without WiFi. */
    } else {
        ESP_LOGI(TAG, "ESP-Hosted ready");
    }

    wifi_config_load();
    wifi_init_softap();
    mdns_init_synth();
    dns_responder_start();
    start_webserver();

    ESP_LOGI(TAG, "ready");
}
