cmake_minimum_required(VERSION 3.20)

project(hongkong_mahjong_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MAHJONG_BUILD_WEB_SERVER "Build the Crow-based HTTP/WebSocket server (requires network for FetchContent)" ON)
option(MAHJONG_BUILD_GUI "Build the Win32 GUI client" ${WIN32})
option(MAHJONG_BUILD_TESTS "Build the unit-test target (tests/test_core.cpp)" ON)

add_library(mahjong_core
  src/core/mahjong_core.cpp
  src/server/room_server.cpp
)

target_include_directories(mahjong_core PUBLIC src)

if(MSVC)
  target_compile_options(mahjong_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(mahjong_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(mahjong_server src/server/main.cpp)
target_link_libraries(mahjong_server PRIVATE mahjong_core)

add_executable(mahjong_client src/client/main.cpp)
target_link_libraries(mahjong_client PRIVATE mahjong_core)

if(MAHJONG_BUILD_GUI AND WIN32)
  add_executable(mahjong_gui WIN32 src/gui/main.cpp)
  target_link_libraries(mahjong_gui PRIVATE mahjong_core user32 gdi32)
endif()

if(MAHJONG_BUILD_TESTS)
  add_executable(mahjong_tests tests/test_core.cpp)
  target_link_libraries(mahjong_tests PRIVATE mahjong_core)

  enable_testing()
  add_test(NAME mahjong_tests COMMAND mahjong_tests)
endif()

if(MAHJONG_BUILD_WEB_SERVER)
  include(FetchContent)

  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
  )

  set(ASIO_CMAKE_ASIO_TAG asio-1-24-0 CACHE STRING "")
  FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-24-0
    GIT_SHALLOW TRUE
  )

  set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "")
  set(CROW_BUILD_TESTS OFF CACHE BOOL "")
  FetchContent_Declare(
    crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.2.0
    GIT_SHALLOW TRUE
  )

  FetchContent_MakeAvailable(nlohmann_json)
  FetchContent_MakeAvailable(asio)

  add_library(asio_standalone INTERFACE)
  target_include_directories(asio_standalone INTERFACE ${asio_SOURCE_DIR}/asio/include)
  target_compile_definitions(asio_standalone INTERFACE ASIO_STANDALONE)
  if(WIN32)
    target_link_libraries(asio_standalone INTERFACE ws2_32 mswsock)
    target_compile_definitions(asio_standalone INTERFACE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(asio_standalone INTERFACE Threads::Threads)
  endif()

  FetchContent_GetProperties(crow)
  if(NOT crow_POPULATED)
    FetchContent_Populate(crow)
  endif()
  add_library(crow_headers INTERFACE)
  target_include_directories(crow_headers INTERFACE ${crow_SOURCE_DIR}/include)
  target_link_libraries(crow_headers INTERFACE asio_standalone)

  add_executable(mahjong_web_server
    src/server/web_main.cpp
    src/server/web_server.cpp
    src/server/json_codec.cpp
  )
  target_link_libraries(mahjong_web_server
    PRIVATE
      mahjong_core
      crow_headers
      nlohmann_json::nlohmann_json
  )
  if(MSVC)
    target_compile_options(mahjong_web_server PRIVATE /bigobj)
  endif()
endif()

