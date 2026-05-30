#include "render_export.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "export";

static FILE    *s_fp        = NULL;
static uint32_t s_frames    = 0;
static bool     s_active    = false;

/* Standard 44-byte PCM WAV header, sizes filled in on stop */
static void write_wav_header(FILE *fp)
{
    uint32_t data_size   = 0;              /* patched on stop */
    uint32_t riff_size   = 36 + data_size; /* patched on stop */
    uint16_t channels    = 2;
    uint32_t sample_rate = 48000;
    uint16_t bits        = 16;
    uint16_t block_align = channels * (bits / 8);
    uint32_t byte_rate   = sample_rate * block_align;
    uint16_t audio_fmt   = 1; /* PCM */

    fwrite("RIFF",    1, 4, fp);
    fwrite(&riff_size,  4, 1, fp);
    fwrite("WAVE",    1, 4, fp);
    fwrite("fmt ",    1, 4, fp);
    uint32_t fmt_sz = 16;
    fwrite(&fmt_sz,     4, 1, fp);
    fwrite(&audio_fmt,  2, 1, fp);
    fwrite(&channels,   2, 1, fp);
    fwrite(&sample_rate,4, 1, fp);
    fwrite(&byte_rate,  4, 1, fp);
    fwrite(&block_align,2, 1, fp);
    fwrite(&bits,       2, 1, fp);
    fwrite("data",    1, 4, fp);
    fwrite(&data_size,  4, 1, fp);
}

bool render_export_start(const char *path)
{
    if (s_active) render_export_stop();

    s_fp = fopen(path, "wb");
    if (!s_fp) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }
    write_wav_header(s_fp);
    s_frames = 0;
    s_active = true;
    ESP_LOGI(TAG, "Export started: %s", path);
    return true;
}

void render_export_write(const int16_t *buf, int frames)
{
    if (!s_active || !s_fp) return;
    fwrite(buf, sizeof(int16_t) * 2, frames, s_fp);
    s_frames += frames;
}

void render_export_stop(void)
{
    if (!s_fp) return;

    uint32_t data_size = s_frames * 2 * sizeof(int16_t);
    uint32_t riff_size = 36 + data_size;

    fseek(s_fp, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, s_fp);
    fseek(s_fp, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, s_fp);

    fclose(s_fp);
    s_fp     = NULL;
    s_active = false;
    ESP_LOGI(TAG, "Export stopped: %lu frames (%.1f s)",
             (unsigned long)s_frames, (double)s_frames / 48000.0);
}

bool render_export_active(void) { return s_active; }

uint32_t render_export_frames(void) { return s_frames; }
