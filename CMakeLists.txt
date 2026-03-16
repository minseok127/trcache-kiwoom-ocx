cmake_minimum_required(VERSION 3.15)
project(kiwoom-ocx-bridge C)

if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows")
endif()

add_subdirectory(trcache)

# trcache's CMakeLists uses CMAKE_SOURCE_DIR for include paths,
# which resolves to this top-level directory when built as a
# subdirectory. Override with the correct paths.
set_property(TARGET trcache PROPERTY INTERFACE_INCLUDE_DIRECTORIES
    "${CMAKE_SOURCE_DIR}/trcache"
    "${CMAKE_SOURCE_DIR}/trcache/src/include"
)

add_subdirectory(engine)
