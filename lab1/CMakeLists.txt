cmake_minimum_required(VERSION 3.16)

project(parallel_lab_1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(parallel_lab
    src/main.cpp
)

# Threads::Threads supplies the appropriate POSIX thread flags on Linux/macOS.
find_package(Threads REQUIRED)
target_link_libraries(parallel_lab PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(parallel_lab PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(parallel_lab PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Native Windows does not provide pthread.h.  The laboratory specifically
# requires pthreads-win32, so point PTHREADS_WIN32_ROOT at an unpacked official
# pthreads-win32 distribution (do not commit its DLL/LIB files to this project):
#
#   cmake -S . -B build -DPTHREADS_WIN32_ROOT=C:/third_party/pthreads-win32
#
# The block locates pthread.h and pthreadVCE2.lib.  If the distribution keeps
# files in a nonstandard layout, the two cache variables below can be set
# explicitly.  PTHREADS_WIN32_DLL is optional, but when set it is copied next
# to the executable so that the program can be run from the build directory.
if(WIN32)
    set(PTHREADS_WIN32_ROOT "" CACHE PATH
        "Root directory of an unpacked pthreads-win32 distribution")
    set(PTHREADS_WIN32_INCLUDE_DIR "" CACHE PATH
        "Directory containing pthread.h (overrides automatic search)")
    set(PTHREADS_WIN32_LIBRARY "" CACHE FILEPATH
        "Full path to pthreadVCE2.lib (overrides automatic search)")
    set(PTHREADS_WIN32_DLL "" CACHE FILEPATH
        "Optional full path to pthreadVCE2.dll to copy beside the executable")

    if(NOT PTHREADS_WIN32_INCLUDE_DIR)
        find_path(PTHREADS_WIN32_INCLUDE_DIR
            NAMES pthread.h
            HINTS "${PTHREADS_WIN32_ROOT}" "${PTHREADS_WIN32_ROOT}/include"
            PATH_SUFFIXES include)
    endif()

    if(NOT PTHREADS_WIN32_LIBRARY)
        # The common layouts are included as hints, but a custom library path
        # can always be supplied through -DPTHREADS_WIN32_LIBRARY=... .
        find_library(PTHREADS_WIN32_LIBRARY
            NAMES pthreadVCE2 pthreadVC2 pthread
            HINTS "${PTHREADS_WIN32_ROOT}"
                  "${PTHREADS_WIN32_ROOT}/lib"
                  "${PTHREADS_WIN32_ROOT}/lib/x64"
                  "${PTHREADS_WIN32_ROOT}/Pre-built.2/lib/x64"
            PATH_SUFFIXES lib lib/x64)
    endif()

    if(NOT PTHREADS_WIN32_INCLUDE_DIR OR NOT PTHREADS_WIN32_LIBRARY)
        message(FATAL_ERROR
            "pthreads-win32 is required for this laboratory on Windows. "
            "Set -DPTHREADS_WIN32_ROOT=<unpacked pthreads-win32 directory>, "
            "or set PTHREADS_WIN32_LIBRARY and ensure pthread.h is discoverable.")
    endif()

    target_include_directories(parallel_lab PRIVATE "${PTHREADS_WIN32_INCLUDE_DIR}")
    target_link_libraries(parallel_lab PRIVATE "${PTHREADS_WIN32_LIBRARY}")

    if(PTHREADS_WIN32_DLL AND EXISTS "${PTHREADS_WIN32_DLL}")
        add_custom_command(TARGET parallel_lab POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PTHREADS_WIN32_DLL}" "$<TARGET_FILE_DIR:parallel_lab>")
    endif()
endif()
