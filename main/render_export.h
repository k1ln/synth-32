#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool     render_export_start(const char *path);
void     render_export_write(const int16_t *buf, int frames);
void     render_export_stop(void);
bool     render_export_active(void);
uint32_t render_export_frames(void);

#ifdef __cplusplus
}
#endif
