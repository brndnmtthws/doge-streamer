cmake_minimum_required(VERSION 3.8)
project(doge-streamer)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules)

include(FindFFmpeg)

find_package(FFmpeg REQUIRED)
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/3rdparty ${OpenCV_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
set(LIBRARIES
    ${OpenCV_LIBS}
    ${FFMPEG_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${FFMPEG_LDFLAGS})

file(GLOB_RECURSE SRC_FILES LIST_DIRECTORIES true ${PROJECT_SOURCE_DIR}/src/*.cc)
file(GLOB PNG_FILES ${PROJECT_SOURCE_DIR}/artwork/*.png)

foreach(FILE ${PNG_FILES})
  configure_file(${FILE} ${PROJECT_BINARY_DIR}/ COPYONLY)
endforeach()

add_executable(doge-streamer ${SRC_FILES})

target_include_directories(doge-streamer PUBLIC ${INCLUDE_DIRS})
target_link_libraries(doge-streamer ${LIBRARIES})

install(TARGETS doge-streamer DESTINATION /opt/doge/bin)
install(FILES ${PNG_FILES} DESTINATION /opt/doge/artwork)

find_program(CLANG_TIDY_EXE
             NAMES "clang-tidy"
             DOC "Path to clang-tidy executable")
if(NOT CLANG_TIDY_EXE)
  message(STATUS "clang-tidy not found.")
else()
  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
  set(DO_CLANG_TIDY "${CLANG_TIDY_EXE}" "-checks=-*,clang-analyzer-*,google-*")
endif()

if(CLANG_TIDY_EXE)
  set_target_properties(doge-streamer
                        PROPERTIES CXX_CLANG_TIDY "${DO_CLANG_TIDY}")
endif()
