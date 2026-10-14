idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.cpp"
		"arduino_compat.cpp"
		"data/enrichment.cpp"
		"data/error_log.cpp"
		"data/fetcher.cpp"
		"data/http_get.cpp"
		"data/http_mutex.cpp"
		"data/storage.cpp"
		"hal/keyboard_input.cpp"
		"hershey/lv_font_hershey.c"
		"ui/alerts.cpp"
		"ui/arrivals_view.cpp"
		"ui/detail_card.cpp"
		"ui/filters.cpp"
		"ui/input_nav.cpp"
		"ui/map_view.cpp"
		"ui/radar_view.cpp"
		"ui/range.cpp"
		"ui/settings.cpp"
		"ui/stats.cpp"
		"ui/stats_view.cpp"
		"ui/status_bar.cpp"
		"ui/views.cpp"
	PRIV_REQUIRES
		esp_lcd
		esp_http_client
		esp_timer
		esp_psram
		esp_netif
		esp_wifi
		fatfs
		nvs_flash
		bt
		esp_driver_tsens
		lvgl
		cjson
		wifi-manager
		badge-bsp
		tanmatsu-wifi
		${extra_requires}
	INCLUDE_DIRS
		"."
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)

# LVGL pulls in main/hershey/lv_font_hershey.h through CONFIG_LV_FONT_CUSTOM_INCLUDE,
# so the directory has to be on every component's include path, not just main's.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/hershey" APPEND)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
