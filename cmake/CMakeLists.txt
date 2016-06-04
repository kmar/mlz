cmake_minimum_required(VERSION 2.6)
project(mlzc)

add_subdirectory(mlz)
include_directories(..)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

add_definitions(-DMLZ_THREADS -DMLZ_COMMANDLINE_TOOL)

set(SOURCE_FILES
    ../mlzc.c
)

add_executable(mlzc ${SOURCE_FILES})

target_link_libraries(mlzc mlz)

if(NOT WIN32)
    target_link_libraries(mlzc pthread)
endif()

add_dependencies(mlzc mlz)
