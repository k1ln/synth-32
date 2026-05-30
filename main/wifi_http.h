#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime WiFi credentials (loaded from NVS or default) */
extern char s_ap_ssid[33];

void wifi_config_load(void);
void wifi_init_softap(void);
void start_webserver(void);

#ifdef __cplusplus
}
#endif
