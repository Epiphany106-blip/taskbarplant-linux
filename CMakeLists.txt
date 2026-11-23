cmake_minimum_required(VERSION 3.20)
project(TaskbarPlant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Gui)
find_package(Qt6 6.4 QUIET COMPONENTS DBus)

# -----------------------------------------------------------------------------
#  版本号
#
#  规则：
#    · 源码正好停在某个 tag 上（正式版 v1.3.0 或预发布 v1.3.0-rc.1）
#      → 直接用那个 tag 的版本号
#    · 其余情况（手动构建、CI 的 main 分支构建……）
#      → 基线版本 + "-canary"
#
#  "canary" 的意思是"这棵树不是任何已发布版本"，一眼能看出手上跑的
#  是不是和 GitHub 上下载的同一个东西。
#
#  判定放在构建期（生成 version.h），不是 configure 期 —— configure 会被
#  CMake 缓存，打完 tag 重新编译不一定触发 configure，版本号会停在旧值。
# -----------------------------------------------------------------------------
set(TBP_BASE_VERSION "1.3.0")
set(TBP_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/tbp_version.h")

# CI 在 tag 构建时显式传入 tag 名（-DTBP_VERSION_OVERRIDE=v1.3.0-rc.2）。
# 不依赖容器内是否有 git —— CI 的 debian:12 容器只装了编译工具链，没有 git，
# 只靠 `git describe` 探测的话，每个发布版都会退化成 canary。
set(TBP_VERSION_OVERRIDE "" CACHE STRING "Explicit version override (CI passes the tag name)")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
# 每次都跑（不要 OUTPUT/DEPENDS 那套增量规则）：
# 增量规则下，头文件一旦生成就不再重跑，打完 tag 重新编译版本号会停在旧值。
# 脚本内部自己比较内容，没变就不写文件，所以不会引起无谓重编。
add_custom_target(tbp_version_header ALL
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT_FILE=${TBP_VERSION_HEADER}
          -DBASE_VERSION=${TBP_BASE_VERSION}
          -DOVERRIDE=${TBP_VERSION_OVERRIDE}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_version.cmake
  BYPRODUCTS "${TBP_VERSION_HEADER}"
  COMMENT "Detecting version"
  VERBATIM)

# kwin_bridge.js 打进 Qt 资源，程序运行时再释放到 ~/.local/share 交给 KWin 加载
add_executable(taskbarplant src/main.cpp src/resources.qrc)
target_link_libraries(taskbarplant PRIVATE Qt6::Widgets Qt6::Gui)
add_dependencies(taskbarplant tbp_version_header)
target_include_directories(taskbarplant PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(Qt6DBus_FOUND)
    target_link_libraries(taskbarplant PRIVATE Qt6::DBus)
    target_compile_definitions(taskbarplant PRIVATE TBP_HAVE_DBUS)
    message(STATUS "Qt6DBus found: KWin window-geometry bridge (Wayland windows) enabled")
else()
    message(STATUS "Qt6DBus not found: only XWayland windows can be used as support")
endif()

# 可选：libX11（xcb / XWayland 会话下还原"停在其它窗口标题栏上"的行为）
find_package(X11 QUIET)
if(X11_FOUND)
    target_compile_definitions(taskbarplant PRIVATE TBP_HAVE_X11)
    target_include_directories(taskbarplant PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(taskbarplant PRIVATE ${X11_LIBRARIES})
    # XShape（点击穿透必需）
    find_library(XEXT_LIB Xext)
    if(XEXT_LIB)
        target_link_libraries(taskbarplant PRIVATE ${XEXT_LIB})
        message(STATUS "X11 + Xext found: window-support tracking and click-through enabled")
    else()
        message(WARNING "Xext not found: input shape (click-through) unavailable")
    endif()
else()
    message(STATUS "X11 not found: pots will rest on the work-area bottom only, no click-through")
endif()

install(TARGETS taskbarplant RUNTIME DESTINATION bin)

# KWin 脚本随程序一起安装（程序会把它写到用户目录再交给 KWin 加载）
