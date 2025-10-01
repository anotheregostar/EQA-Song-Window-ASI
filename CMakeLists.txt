cmake_minimum_required(VERSION 3.20)
project(eqa_buffsong_asi LANGUAGES CXX)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)

include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

add_library(eqa_buffsong_asi SHARED
  src/eqa_buffsong_asi.cpp
)

target_compile_features(eqa_buffsong_asi PRIVATE cxx_std_20)
target_link_libraries(eqa_buffsong_asi PRIVATE minhook)
target_compile_definitions(eqa_buffsong_asi PRIVATE _CRT_SECURE_NO_WARNINGS)

target_include_directories(eqa_buffsong_asi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# emit .asi alongside the DLL
add_custom_command(TARGET eqa_buffsong_asi POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:eqa_buffsong_asi> $<TARGET_FILE_DIR:eqa_buffsong_asi>/eqa_buffsong.asi
)
