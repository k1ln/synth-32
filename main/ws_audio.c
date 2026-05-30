/*
 * ws_audio.c — WebSocket audio streaming (M8).
 *
 * Architecture
 * ───────────────────────────────────────────────────────────────────────────
 *  audio_task (core 1)          ws_audio_task (core 0)
 *  ──────────────────           ──────────────────────
 *  ws_audio_push()              drains ring head→tail
 *   writes interleaved          packs 256-frame blocks
 *   int16 into ring             sends binary WS frames
 *
 * Ring buffer: PSRAM, 8 KB = 2048 int16 stereo samples (1024 frames).
 * Block size:  256 frames = 512 int16 = 1024 bytes per WS frame.
 *
 * PA mute: GPIO_PA is driven low while a client holds the audio stream.
 *
 * Only one audio client at a time.  New client evicts old client:
 *   1. Send WS_MSG_AUDIO_DROPPED to old fd.
 *   2. Free old slot.
 *   3. Claim new slot, mute PA, reset ring.
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "audio.h"
#include "ws_server.h"
#include "ws_audio.h"

static const char *TAG = "ws_audio";

/* ── Constants ───────────────────────────────────────────────────────────── */
#define WS_AUDIO_RING_FRAMES   1024   /* must be power of 2 */
#define WS_AUDIO_BLOCK_FRAMES   256   /* frames per WS binary frame         */
#define WS_AUDIO_RING_SAMPLES  (WS_AUDIO_RING_FRAMES * 2)  /* stereo int16 */
#define WS_AUDIO_BLOCK_SAMPLES (WS_AUDIO_BLOCK_FRAMES * 2)
#define WS_AUDIO_BLOCK_BYTES   (WS_AUDIO_BLOCK_SAMPLES * sizeof(int16_t))

/* ── Ring buffer ─────────────────────────────────────────────────────────── */
static int16_t    *s_ring    = NULL;   /* PSRAM, WS_AUDIO_RING_SAMPLES */
static volatile uint32_t s_head = 0;  /* written by audio_task (samples) */
static volatile uint32_t s_tail = 0;  /* read by ws_audio_task  (samples) */

static inline uint32_t ring_avail(void)
{
    return s_head - s_tail;  /* monotonic counters; wraps safely as uint32 */
}

/* ── Client state ────────────────────────────────────────────────────────── */
static volatile int  s_client_fd = -1;
static SemaphoreHandle_t s_mx;         /* protects s_client_fd transitions */

/* ── PA mute helpers ─────────────────────────────────────────────────────── */
static void pa_mute(void)
{
    gpio_set_level(BSP_POWER_AMP_IO, 0);
}

static void pa_unmute(void)
{
    gpio_set_level(BSP_POWER_AMP_IO, 1);
}

/* ── Drop existing client ────────────────────────────────────────────────── */
static void drop_client_locked(void)
{
    if (s_client_fd < 0) return;
    uint8_t msg[1] = { WS_MSG_AUDIO_DROPPED };
    ws_send_to_fd(s_client_fd, msg, sizeof(msg));
    s_client_fd = -1;
    pa_unmute();
    ESP_LOGI(TAG, "audio stream dropped");
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ws_audio_start(int fd)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    drop_client_locked();          /* evict any existing client */
    s_client_fd = fd;
    /* Reset ring so ws_audio_task starts fresh */
    s_head = 0;
    s_tail = 0;
    pa_mute();
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "audio stream started fd=%d", fd);
}

void ws_audio_stop(int fd)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_client_fd == fd || fd == -1) {
        s_client_fd = -1;
        pa_unmute();
        ESP_LOGI(TAG, "audio stream stopped fd=%d", fd);
    }
    xSemaphoreGive(s_mx);
}

void ws_audio_client_closed(int fd)
{
    ws_audio_stop(fd);
}

/* ── Push from audio task (core 1) ──────────────────────────────────────── */

void ws_audio_push(const int16_t *buf, int n_frames)
{
    if (s_client_fd < 0 || !s_ring) return;

    int n_samples = n_frames * 2;
    uint32_t head = s_head;

    /* If ring is more than half full the reader is falling behind — skip
     * this block rather than overwriting unread data with corrupted audio. */
    if (head - s_tail > (uint32_t)(WS_AUDIO_RING_SAMPLES / 2)) return;

    for (int i = 0; i < n_samples; i++) {
        s_ring[head & (WS_AUDIO_RING_SAMPLES - 1)] = buf[i];
        head++;
    }
    s_head = head;   /* atomic store on RISC-V (aligned uint32) */
}

/* ── Drain task (core 0) ─────────────────────────────────────────────────── */

static void ws_audio_task(void *arg)
{
    (void)arg;
    /* Heap-allocate the send frame so it survives across iterations and
     * is in regular RAM (not stack) for reliable DMA/TCP access. */
    uint8_t *s_frame = (uint8_t *)heap_caps_malloc(1 + WS_AUDIO_BLOCK_BYTES, MALLOC_CAP_DEFAULT);
    if (!s_frame) {
        ESP_LOGE(TAG, "frame alloc failed");
        vTaskDelete(NULL);
        return;
    }
    s_frame[0] = 0x10;  /* message type: raw PCM audio frame */

    while (1) {
        int fd = s_client_fd;
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Spin-wait in small steps until a full block is available */
        while (ring_avail() < (uint32_t)WS_AUDIO_BLOCK_SAMPLES) {
            if (s_client_fd < 0) goto next_iter;
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* Copy one block from ring */
        {
            uint32_t tail = s_tail;
            int16_t *dst  = (int16_t *)(s_frame + 1);
            for (int i = 0; i < WS_AUDIO_BLOCK_SAMPLES; i++) {
                dst[i] = s_ring[tail & (WS_AUDIO_RING_SAMPLES - 1)];
                tail++;
            }
            s_tail = tail;
        }

        fd = s_client_fd;   /* re-read after wait */
        if (fd < 0) continue;

        /* Send directly via httpd to avoid per-frame mutex overhead */
        {
            httpd_handle_t hd = ws_server_handle();
            if (hd) {
                httpd_ws_frame_t frm = {
                    .type       = HTTPD_WS_TYPE_BINARY,
                    .payload    = s_frame,
                    .len        = 1 + WS_AUDIO_BLOCK_BYTES,
                    .final      = true,
                    .fragmented = false,
                };
                esp_err_t err = httpd_ws_send_frame_async(hd, fd, &frm);
                if (err != ESP_OK) {
                    ESP_LOGD(TAG, "send err %d, stopping stream", err);
                    ws_audio_stop(fd);
                }
            }
        }
next_iter:;
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void ws_audio_init(void)
{
    s_ring = (int16_t *)heap_caps_malloc(
        WS_AUDIO_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ESP_LOGE(TAG, "PSRAM alloc failed for audio ring");
        return;
    }
    s_mx   = xSemaphoreCreateMutex();
    s_head = 0;
    s_tail = 0;

    xTaskCreatePinnedToCore(ws_audio_task, "ws_audio", 4096, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "ws_audio init: ring %u bytes in PSRAM",
             (unsigned)(WS_AUDIO_RING_SAMPLES * sizeof(int16_t)));
}
