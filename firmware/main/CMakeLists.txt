idf_component_register(
    SRCS "app_main.c"
         "ipradio_state.c"
         "ipradio_input.c"
         "ipradio_storage.c"
         "ipradio_tuner.c"
         "ipradio_audio.c"
         "ipradio_net.c"
         "ipradio_ui.c"
         "ipradio_netradio.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_i2s esp_timer
             esp_psram fatfs sdmmc esp_driver_sdmmc json
             esp_wifi esp_netif esp_event nvs_flash lwip
             esp_http_client esp-tls
             lvgl__lvgl
             audio_pipeline audio_stream esp-adf-libs audio_sal
)
