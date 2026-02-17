cmake_minimum_required(VERSION 3.24)
project(hl2mp-maps-downloader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(hl2mp-maps-downloader
  main.cpp
  main.hpp
  app.rc
)

target_compile_features(hl2mp-maps-downloader PRIVATE cxx_std_23)

target_compile_definitions(hl2mp-maps-downloader PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

if (MSVC)
  target_compile_options(hl2mp-maps-downloader PRIVATE /W4 /permissive-)
else()
  target_compile_options(hl2mp-maps-downloader PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(CURL CONFIG REQUIRED)
find_package(BZip2 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)

target_link_libraries(hl2mp-maps-downloader PRIVATE
  CURL::libcurl
  BZip2::BZip2
  nlohmann_json::nlohmann_json
  ftxui::screen
  ftxui::dom
  ftxui::component
)
