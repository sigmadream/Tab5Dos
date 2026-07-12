cmake_minimum_required(VERSION 3.20)
project(tabdos_regression_tests LANGUAGES CXX)

enable_testing()

set(TABDOS_PC_DIR "${CMAKE_CURRENT_LIST_DIR}/../components/tabdos_pc")
add_library(tabdos_pc_host STATIC
    "${TABDOS_PC_DIR}/pc/pc_bios.cpp"
    "${TABDOS_PC_DIR}/pc/pc_disk_catalog.cpp"
    "${TABDOS_PC_DIR}/pc/pc_disk_image.cpp"
    "${TABDOS_PC_DIR}/pc/pc_font_8x16.cpp"
    "${TABDOS_PC_DIR}/pc/pc_i8086.cpp"
    "${TABDOS_PC_DIR}/pc/pc_keyboard_controller.cpp"
    "${TABDOS_PC_DIR}/pc/pc_machine.cpp"
    "${TABDOS_PC_DIR}/pc/pc_opl2.cpp"
    "${TABDOS_PC_DIR}/pc/pc_text_renderer.cpp"
)
target_include_directories(tabdos_pc_host PUBLIC
    "${TABDOS_PC_DIR}"
    "${TABDOS_PC_DIR}/pc"
    "${CMAKE_CURRENT_LIST_DIR}/../main"
)
target_compile_features(tabdos_pc_host PUBLIC cxx_std_20)

function(add_tabdos_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE tabdos_pc_host)
  # These imported tests use assert() as their test primitive. Keep assertions
  # active in optimized configurations so Release runs execute the checks too.
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_tabdos_test(tabdos_regression_tests regression_tests.cpp)
add_tabdos_test(pc_disk_catalog_test pc_disk_catalog_test.cpp)
add_tabdos_test(pc_disk_image_test pc_disk_image_test.cpp)
add_tabdos_test(pc_keyboard_controller_test pc_keyboard_controller_test.cpp)
add_tabdos_test(pc_machine_test pc_machine_test.cpp)
add_tabdos_test(pc_text_renderer_test pc_text_renderer_test.cpp)
