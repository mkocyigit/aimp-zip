cmake_minimum_required(VERSION 3.21)

project(AimpZipPlugin LANGUAGES C CXX)

include(FetchContent)

if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_Declare(
  miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
)
FetchContent_GetProperties(miniz)
if(NOT miniz_POPULATED)
  FetchContent_Populate(miniz)
endif()

add_library(aimp_zip SHARED
  src/ZipPlugin.cpp
  sdk/Sources/Cpp/apiTypes.cpp
  sdk/Sources/Cpp/Helpers/apiWrappers.cpp
)

target_compile_features(aimp_zip PRIVATE cxx_std_17)

target_include_directories(aimp_zip PRIVATE
  src
  sdk/Sources/Cpp
  sdk/Sources/Cpp/Helpers
  ${miniz_SOURCE_DIR}
)

target_sources(aimp_zip PRIVATE
  ${miniz_SOURCE_DIR}/miniz.c
  ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_zip.c
)

target_compile_definitions(aimp_zip PRIVATE
  NOMINMAX
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
  target_compile_options(aimp_zip PRIVATE /W4 /permissive-)
endif()

set_target_properties(aimp_zip PROPERTIES
  OUTPUT_NAME "aimp_zip"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
