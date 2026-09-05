idf_component_register(
    SRCS "app_main.c"
         "ipradio_watchdog.c"
         "ipradio_state.c"
         "ipradio_input.c"
         "ipradio_storage.c"
         "ipradio_tuner.c"
         "ipradio_audio.c"
         "ipradio_net.c"
         "ipradio_ui.c"
         "ipradio_adf_stack.c"
         "ipradio_board_codec.c"
         "ipradio_netradio.c"
         "ipradio_bridge.c"
         "ipradio_fonts.c"
         "ipradio_ui_theme.c"
         "ipradio_ui_dialog.c"
         "ipradio_ui_idle.c"
         "ipradio_ui_menu.c"
         "ipradio_ui_tune.c"
         "ipradio_ui_keyboard.c"
         "ipradio_ui_wifi.c"
         "ipradio_ui_search.c"
         "ipradio_ui_stations.c"
         "ipradio_ui_diag.c"
         "ipradio_ui_brightness.c"
         "ipradio_ui_clock.c"
         "ipradio_ui_netbadge.c"
    INCLUDE_DIRS "."
    # Шрифты кладутся в образ как есть и рендерятся на плате
    # (lv_tiny_ttf). Урезаны до латиницы с кириллицей:
    # tools/subset_font.py, лицензия - assets/DejaVu-LICENSE.txt.
    EMBED_FILES "assets/dejavu_ru.ttf"
                "assets/dejavu_ru_bold.ttf"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_i2s esp_timer
             esp_psram fatfs sdmmc esp_driver_sdmmc json
             esp_wifi esp_netif esp_event nvs_flash lwip
             esp_http_client esp-tls
             lvgl__lvgl
             waveshare__esp32_p4_wifi6_touch_lcd_5
             audio_pipeline audio_stream esp-adf-libs audio_sal
             esp_codec_dev

    # Без этого наш ipradio_adf_stack.c в образ не попадает.
    #
    # Единственное, что он определяет - xTaskCreateRestrictedPinnedToCore.
    # В ADF эта функция объявлена слабой, и её заглушку компоновщик
    # находит в libaudio_sal.a раньше, чем добирается до нашей: наш
    # объектный файл из архива не выдёргивается вовсе, потому что
    # больше на него никто не ссылается. WHOLE_ARCHIVE укладывает
    # в образ все объектные файлы компонента, и тогда сильное
    # определение побеждает слабое, как и положено.
    WHOLE_ARCHIVE
)
