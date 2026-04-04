project(DarkTanos LANGUAGES CXX)

find_package(JNI REQUIRED)

include_directories(${PROJECT_NAME} PRIVATE ${JNI_INCLUDE_DIRS})

add_definitions(-DAPI_VERSION=${API_VERSION})

set(BROWSER_APPIMAGE_PATH "${CMAKE_SOURCE_DIR}/browser/dist/darkbot_browser_linux.AppImage")
if(NOT EXISTS "${BROWSER_APPIMAGE_PATH}")
    message(FATAL_ERROR "Required browser AppImage not found: ${BROWSER_APPIMAGE_PATH}")
endif()

execute_process(
    COMMAND sh -c "sha256sum '${BROWSER_APPIMAGE_PATH}' | awk '{print \$1}'"
    OUTPUT_VARIABLE BROWSER_APPIMAGE_SHA256
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _browser_appimage_sha256_err
    RESULT_VARIABLE _browser_appimage_sha256_result
)
if(NOT _browser_appimage_sha256_result EQUAL 0)
    message(FATAL_ERROR "Failed to compute SHA256 for ${BROWSER_APPIMAGE_PATH}: ${_browser_appimage_sha256_err}")
endif()
string(STRIP "${BROWSER_APPIMAGE_SHA256}" BROWSER_APPIMAGE_SHA256)
string(LENGTH "${BROWSER_APPIMAGE_SHA256}" _browser_appimage_sha256_len)
if(NOT _browser_appimage_sha256_len EQUAL 64)
    message(FATAL_ERROR "SHA256 output for ${BROWSER_APPIMAGE_PATH} is invalid: ${BROWSER_APPIMAGE_SHA256}")
endif()

add_library(${PROJECT_NAME} SHARED
    eu_darkbot_api_DarkTanos.cpp
    bot_client.cpp
    proc_util.cpp
    sock_ipc.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE BROWSER_APPIMAGE_SHA256="${BROWSER_APPIMAGE_SHA256}")

find_package(X11 REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${X11_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE ${X11_LIBRARIES})

target_compile_options(${PROJECT_NAME} PRIVATE -std=c++17)
