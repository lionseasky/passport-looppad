idf_component_register(
    SRCS "main.c"
         "loop_core.c"
         "loop_store.c"
         "loop_audio.c"
         "loop_font.c"
         "loop_ui.c"
         "loop_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer esp_partition
)

# 生成的 16/24/32 px 中文字库(见 tools/loop_font.py 与 assets/fonts/loop_charset.txt)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/loop_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/loop_font_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/loop_font_32.c"
)
