project(linux-alsa)

find_package(ALSA)
if(NOT ALSA_FOUND)
	message(STATUS "ALSA not found, disabling ALSA plugin")
	return()
endif()

include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/libobs")

set(linux-alsa_SOURCES
	linux-alsa.c
	alsa-input.c
)

add_library(linux-alsa MODULE
	${linux-alsa_SOURCES}
)
target_link_libraries(linux-alsa
	libobs
	${ALSA_LIBRARY}
)

install_obs_plugin_with_data(linux-alsa data)
