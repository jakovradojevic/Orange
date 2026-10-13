set(ORANGE_FILES Orange.c Orange.rc)

add_executable(Orange WIN32 ${ORANGE_FILES})

set(ORANGE_RC_ASSETS
  100.ico
  101.bmp 102.bmp 103.bmp 104.bmp 105.bmp 106.bmp
  107.bmp 108.bmp 109.bmp 110.bmp 111.bmp
  113.wav 114.wav 115.wav 116.wav 117.wav 118.wav
  Orange.manifest
)
list(TRANSFORM ORANGE_RC_ASSETS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set_source_files_properties(Orange.rc PROPERTIES OBJECT_DEPENDS "${ORANGE_RC_ASSETS}")

target_compile_definitions(Orange PRIVATE UNICODE _UNICODE)
target_link_libraries(Orange PRIVATE user32 gdi32 winmm shell32 comctl32 dwmapi)

if(MSVC)
  set_property(TARGET Orange PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET Orange PROPERTY VS_DPI_AWARE "ON")
else()
  target_compile_options(Orange PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_custom_command(TARGET Orange POST_BUILD COMMAND ${CMAKE_STRIP} -s $<TARGET_FILE:Orange>)
  endif()
endif()
