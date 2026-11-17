add_library(tmux-platform STATIC argv.c discovery.c pty-win32.c win32-util.c remote.c)
target_include_directories(tmux-platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tmux-platform PUBLIC advapi32)
