add_executable(kob_engine main.c)

target_include_directories(kob_engine PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(kob_engine PRIVATE trcache)

if(MSVC)
    target_compile_options(kob_engine PRIVATE /W4)
else()
    # MinGW on Windows
    target_compile_options(kob_engine PRIVATE -Wall -Wextra)
endif()
