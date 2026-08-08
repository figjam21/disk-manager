cmake_minimum_required(VERSION 3.16)
project(disk-manager CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(Curses REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(PkgConfig)

option(BUILD_GUI "Build the gtkmm desktop GUI (requires libgtkmm-3.0-dev)" ON)
set(HAVE_GTKMM FALSE)
if(BUILD_GUI AND PkgConfig_FOUND)
  pkg_check_modules(GTKMM gtkmm-3.0)
  if(GTKMM_FOUND)
    set(HAVE_GTKMM TRUE)
  else()
    message(WARNING "libgtkmm-3.0-dev not found -- skipping disk-manager-gui (TUI will still build)")
  endif()
endif()

add_library(dm_core STATIC
  src/core/command.cpp
  src/core/device.cpp
  src/core/smart.cpp
  src/core/partition.cpp
  src/core/filesystem.cpp
  src/core/mdadm.cpp
  src/core/locate.cpp
  src/core/firmware.cpp
  src/core/recovery.cpp
)
target_include_directories(dm_core PUBLIC include)
target_link_libraries(dm_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)

add_executable(disk-manager-tui
  src/tui/main.cpp
  src/tui/tui_app.cpp
  src/tui/ui_helpers.cpp
)
target_include_directories(disk-manager-tui PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(disk-manager-tui PRIVATE dm_core ${CURSES_LIBRARIES})

if(HAVE_GTKMM)
  add_executable(disk-manager-gui
    src/gui/main.cpp
    src/gui/gui_app.cpp
  )
  target_include_directories(disk-manager-gui PRIVATE ${GTKMM_INCLUDE_DIRS})
  target_link_directories(disk-manager-gui PRIVATE ${GTKMM_LIBRARY_DIRS})
  target_link_libraries(disk-manager-gui PRIVATE dm_core ${GTKMM_LIBRARIES})
  target_compile_options(disk-manager-gui PRIVATE ${GTKMM_CFLAGS_OTHER})
endif()

install(TARGETS disk-manager-tui RUNTIME DESTINATION bin)
if(HAVE_GTKMM)
  install(TARGETS disk-manager-gui RUNTIME DESTINATION bin)
endif()
