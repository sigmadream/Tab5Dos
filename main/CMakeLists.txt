idf_component_register(
    SRCS
        "main.cpp"
        "tab5_lcd_text.cpp"
        "tab5_runtime_log.cpp"
        "tab5_speaker.cpp"
        "tab5_usb_keyboard.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        tabdos_pc
        m5stack_tab5_noglib
        usb_host_hid
        esp_lcd
        esp_lcd_touch
        esp_driver_gpio
        esp_driver_ppa
        esp_codec_dev
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

if(DEFINED ENV{TABDOS_AUTOTEST_KEYS})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TABDOS_AUTOTEST_KEYS=1)
endif()
