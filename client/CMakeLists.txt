project(DarkTanos LANGUAGES CXX)

find_package(JNI REQUIRED)

include_directories(${PROJECT_NAME} PRIVATE ${JNI_INCLUDE_DIRS})

add_definitions(-DAPI_VERSION=${API_VERSION})

add_library(${PROJECT_NAME} SHARED
    eu_darkbot_api_DarkTanos.cpp
    bot_client.cpp
    proc_util.cpp
    sock_ipc.cpp
)

find_package(X11 REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${X11_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE ${X11_LIBRARIES})

target_compile_options(${PROJECT_NAME} PRIVATE -std=c++17)
