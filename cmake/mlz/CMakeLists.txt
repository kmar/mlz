cmake_minimum_required(VERSION 2.6)
project(mlz)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

add_definitions(-DMLZ_THREADS)
include_directories(../..)

set(SOURCE_FILES
    ../../mlz_common.h
    ../../mlz_dec.c
    ../../mlz_dec.h
    ../../mlz_enc.c
    ../../mlz_enc.h
    ../../mlz_stream_common.h
    ../../mlz_stream_dec.c
    ../../mlz_stream_dec.h
    ../../mlz_stream_enc.c
    ../../mlz_stream_enc.h
    ../../mlz_thread.c
    ../../mlz_thread.h
)
add_library(mlz STATIC ${SOURCE_FILES})
