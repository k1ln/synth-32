# Regenerate the embedded sound manifest before every build.
# gen_manifest.py hashes main/sounds/ (converting non-WAV via ffmpeg)
# and writes sounds_manifest.json which is then baked into the firmware.
# To skip the manifest update (faster incremental builds): set env var
#   SYNTH32_SKIP_MANIFEST=1
if(NOT DEFINED ENV{SYNTH32_SKIP_MANIFEST})
    execute_process(
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_manifest.py
        RESULT_VARIABLE _manifest_ret
    )
    if(NOT _manifest_ret EQUAL 0)
        message(WARNING "gen_manifest.py failed (exit ${_manifest_ret}) — using stale manifest")
    endif()
endif()

idf_component_register(SRCS "mdns_init.c" "sdcard.c" "sounds_http.c" "synth-32.c" "wav_player.c"
                             "gfx.c" "audio.c" "wifi_http.c" "ui.c" "ui_draw.c" "ui_screens.c"
                             "ws_server.c" "ws_cmd.c" "ws_audio.c"
                             "clock.c" "lane.c"
                             "drum_seq.c" "piano_roll.c"
                             "synth_voice.c" "synth_osc.c" "synth_phys.c" "synth_extra.c"
                             "arp.c" "lfo.c" "fx.c"
                             "settings.c" "song.c" "render_export.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "sounds_manifest.json"
                    EMBED_FILES   "www/index.html"
                                  "www/app.css"
                                  "www/app.js"
                    REQUIRES
                        esp_timer
                        nvs_flash
                        esp_wifi
                        esp_event
                        esp_netif
                        esp_http_server
                        esp_driver_i2s
                        esp_driver_gpio
                        esp_driver_i2c
                        esp_hw_support
                        esp_mm
                        esp_driver_ledc
                        esp_lcd
                        fatfs
                        espressif__esp_codec_dev
                        waveshare__esp32_p4_platform
                        espressif__esp_lcd_touch
                        espressif__esp_lcd_touch_gt911
                        espressif__mdns
                        )
