# -----------------------------------------------------------------------------
#  生成 version.h
#
#  由构建系统调用（add_custom_command），每次编译都会重新跑一遍。
#  判定规则：
#    · HEAD 正好停在一个 tag 上（正式版 v1.3.0 或预发布 v1.3.0-rc.1）
#      → 用那个 tag 的版本号
#    · 其余情况（手动构建、CI 的 main 分支构建……）
#      → 基线版本 + "-canary"
#
#  写成构建期生成而不是 configure 期求值，是因为 configure 会被 CMake 缓存：
#  打完 tag 再重新编译，configure 不一定重跑，版本号就会停留在旧值。
# -----------------------------------------------------------------------------

if(NOT DEFINED BASE_VERSION)
  message(FATAL_ERROR "gen_version.cmake: 缺少 BASE_VERSION")
endif()
if(NOT DEFINED OUT_FILE)
  message(FATAL_ERROR "gen_version.cmake: 缺少 OUT_FILE")
endif()

set(_ver "${BASE_VERSION}-canary")

find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE _tag
    RESULT_VARIABLE _rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_rc EQUAL 0 AND _tag MATCHES "^v(.+)$")
    set(_ver "${CMAKE_MATCH_1}")
  endif()
endif()

# 只保留 [0-9A-Za-z.+-]，防止 tag 里出现奇怪字符把 C 字符串弄坏
string(REGEX REPLACE "[^0-9A-Za-z.+-]" "_" _ver "${_ver}")

file(WRITE "${OUT_FILE}"
"// 由 cmake/gen_version.cmake 自动生成，请勿手改。\n"
"#pragma once\n"
"#define TBP_VERSION \"${_ver}\"\n")

message(STATUS "TaskbarPlant version: ${_ver}")
