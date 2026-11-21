cmake_minimum_required(VERSION 3.20)
project(TaskbarPlant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Gui)
find_package(Qt6 6.4 QUIET COMPONENTS DBus)

# kwin_bridge.js 打进 Qt 资源，程序运行时再释放到 ~/.local/share 交给 KWin 加载
add_executable(taskbarplant src/main.cpp src/resources.qrc)
target_link_libraries(taskbarplant PRIVATE Qt6::Widgets Qt6::Gui)

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
