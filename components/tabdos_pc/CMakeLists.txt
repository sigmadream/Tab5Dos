set(TABDOS_PC_ROOT "${CMAKE_CURRENT_LIST_DIR}/pc")

idf_component_register(
    SRCS
        "${TABDOS_PC_ROOT}/pc_bios.cpp"
        "${TABDOS_PC_ROOT}/pc_disk_catalog.cpp"
        "${TABDOS_PC_ROOT}/pc_disk_image.cpp"
        "${TABDOS_PC_ROOT}/pc_font_8x16.cpp"
        "${TABDOS_PC_ROOT}/pc_i8086.cpp"
        "${TABDOS_PC_ROOT}/pc_keyboard_controller.cpp"
        "${TABDOS_PC_ROOT}/pc_machine.cpp"
        "${TABDOS_PC_ROOT}/pc_opl2.cpp"
        "${TABDOS_PC_ROOT}/pc_text_renderer.cpp"
    INCLUDE_DIRS
        "${TABDOS_PC_ROOT}"
        "${CMAKE_CURRENT_LIST_DIR}"
    REQUIRES fatfs
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
