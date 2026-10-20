set(FM_FONT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/flightmate_ui_16.c")

idf_component_register(
    SRCS "main.c"
         "efuse_guard.c"
         "flightmate_logic.c"
         "flightmate_store.c"
         "flightmate_wifi.c"
         "flightmate_net.c"
         "flightmate_ui.c"
         "airband_audio.c"
         "airband_logic.c"
         "airband_stations.c"
         "airband_extra_ca.c"
         "${FM_FONT_SOURCE}"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi json mbedtls nvs_flash
             minimp3 esp_libhelix_mp3
)

# FlightMate never writes eFuse. Replace the SDK startup's BLOCK0 coding-error
# check (the only code that touches eFuse registers besides the mandatory MAC /
# ADC-calibration reads) with the no-op in efuse_guard.c, so the real function
# and its register-level diagnostics are dropped from the image.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_efuse_check_errors")
