cmake_minimum_required(VERSION 3.16)
project(TooD VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
find_package(ZLIB REQUIRED)

add_executable(tood
  src/main.cpp
  src/model.h       src/model.cpp
  src/solver.h      src/solver.cpp
  src/gifwriter.h   src/gifwriter.cpp
  src/canvas.h      src/canvas.cpp
  src/mainwindow.h  src/mainwindow.cpp
)

target_compile_options(tood PRIVATE -Wall -Wextra)
target_link_libraries(tood PRIVATE Qt6::Widgets Qt6::Svg ZLIB::ZLIB)
