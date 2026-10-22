cmake_minimum_required(VERSION 3.21)
project(alpha_news_backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(ALFA_CORE_SOURCES
    src/reader.cpp
    src/news.cpp
    src/data_store.cpp
    src/analytics.cpp
    src/llm_client.cpp
    src/agent.cpp
    src/http_server.cpp
)
add_executable(alpha_backend src/main.cpp ${ALFA_CORE_SOURCES})
target_include_directories(alpha_backend PRIVATE src)
target_link_libraries(alpha_backend PRIVATE CURL::libcurl Threads::Threads)
if(WIN32)
    target_link_libraries(alpha_backend PRIVATE ws2_32)
endif()
if(MSVC)
    target_compile_options(alpha_backend PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(alpha_backend PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(ALFA_BUILD_EXAMPLES "Build the C++ API example client" ON)
if(ALFA_BUILD_EXAMPLES)
    add_executable(AlphaExampleClient example/client.cpp)
    target_link_libraries(AlphaExampleClient PRIVATE CURL::libcurl)
    if(MINGW)
        target_link_options(AlphaExampleClient PRIVATE -municode)
    endif()
    if(MSVC)
        target_compile_options(AlphaExampleClient PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(AlphaExampleClient PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(WIN32)
    add_executable(AlphaPdfOcr tools/windows_ocr.cpp)
    target_link_libraries(AlphaPdfOcr PRIVATE windowsapp)
    if(MSVC)
        target_compile_options(AlphaPdfOcr PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(analytics_index_tests tests/analytics_index_tests.cpp src/analytics.cpp)
    target_include_directories(analytics_index_tests PRIVATE src)
    if(MSVC)
        target_compile_options(analytics_index_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME analytics_index COMMAND analytics_index_tests)
    add_executable(backend_tests tests/backend_tests.cpp ${ALFA_CORE_SOURCES})
    target_include_directories(backend_tests PRIVATE src tests)
    target_link_libraries(backend_tests PRIVATE CURL::libcurl Threads::Threads)
    if(MSVC)
        target_compile_options(backend_tests PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME backend COMMAND backend_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(WIN32 AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    file(GLOB CURL_RUNTIME_DLLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll")
    foreach(runtime_dll IN LISTS CURL_RUNTIME_DLLS)
        add_custom_command(TARGET alpha_backend POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime_dll}" "$<TARGET_FILE_DIR:alpha_backend>"
            VERBATIM)
        if(ALFA_BUILD_EXAMPLES)
            add_custom_command(TARGET AlphaExampleClient POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime_dll}" "$<TARGET_FILE_DIR:AlphaExampleClient>"
                VERBATIM)
        endif()
    endforeach()
endif()

