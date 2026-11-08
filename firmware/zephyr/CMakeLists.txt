cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mraf)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WEBUI_HEADER ${CMAKE_CURRENT_BINARY_DIR}/webui.h)
add_custom_command(
  OUTPUT ${WEBUI_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen_webui.py
          --zephyr --out ${WEBUI_HEADER}
  DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen_webui.py
    ${CMAKE_CURRENT_SOURCE_DIR}/../../web/index.html
  COMMENT "Generating web UI header"
  VERBATIM
)
add_custom_target(mraf_webui DEPENDS ${WEBUI_HEADER})

zephyr_include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ArduinoJson
)

zephyr_compile_definitions(
  ARDUINOJSON_ENABLE_ARDUINO_STRING=0
  ARDUINOJSON_ENABLE_STD_STRING=0
  ARDUINOJSON_ENABLE_STD_STREAM=0
  ARDUINOJSON_ENABLE_EXCEPTIONS=0
  ARDUINOJSON_ENABLE_RTTI=0
  ARDUINOJSON_ENABLE_PROGMEM=0
)

set(MRAF_SOURCES
  src/api.cpp
  src/coap_client.cpp
  src/controller.cpp
  src/main.cpp
  src/remote.cpp
  src/settings.cpp
  src/state.cpp
)

if(CONFIG_ARCH_POSIX)
  list(APPEND MRAF_SOURCES src/platform_native.cpp)
else()
  list(APPEND MRAF_SOURCES src/platform_esp32.cpp)
endif()

target_sources(app PRIVATE ${MRAF_SOURCES})

zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_mraf_http
  KVMA RAM_REGION GROUP RODATA_REGION)
add_dependencies(app mraf_webui)
