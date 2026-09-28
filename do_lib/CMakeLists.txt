project(do_lib LANGUAGES CXX)

include_directories(../third_party/)

add_library(${PROJECT_NAME} SHARED
    do_lib_linux.cpp
    disassembler.cpp
    ipc.cpp
    ipc_handler.cpp
    darkorbit.cpp
    memory.cpp
    avm.cpp
    singleton.cpp
    flash_stuff.cpp
)
target_compile_options(${PROJECT_NAME} PUBLIC -Wall)

target_link_libraries(${PROJECT_NAME} subhook pthread ${CMAKE_DL_LIBS})


# only the LD_PRELOAD interposer is exported; nothing of ours may clash with symbols in the browser
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
