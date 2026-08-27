idf_component_register(
    SRCS "app_main.c"
         "ipradio_state.c"
         "ipradio_input.c"
         "ipradio_storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer esp_psram
             fatfs sdmmc esp_driver_sdmmc json
)
