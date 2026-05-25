cmake_minimum_required(VERSION 3.20)
project(oysyn_search CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# === Dependencies ===

# cpp-httplib (header-only HTTP server)
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
)
FetchContent_MakeAvailable(httplib)

# nlohmann/json (header-only JSON)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# pugixml (XML parser)
FetchContent_Declare(pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
)
FetchContent_MakeAvailable(pugixml)

# === libcurl: download headers only, link to system .so ===
FetchContent_Declare(curl_headers
    URL https://github.com/curl/curl/releases/download/curl-8_5_0/curl-8.5.0.tar.xz
)
FetchContent_GetProperties(curl_headers)
if(NOT curl_headers_POPULATED)
    FetchContent_Populate(curl_headers)
endif()
set(CURL_INCLUDE_DIR "${curl_headers_SOURCE_DIR}/include")

find_file(CURL_LIBRARY NAMES libcurl.so.4 libcurl.so
    HINTS /usr/lib/x86_64-linux-gnu /usr/lib
)
if(NOT CURL_LIBRARY)
    message(FATAL_ERROR "libcurl.so not found. Install: sudo apt-get install libcurl4")
endif()

message(STATUS "CURL library: ${CURL_LIBRARY}")
message(STATUS "CURL include: ${CURL_INCLUDE_DIR}")

# === Build ===
add_executable(oysyn_search main.cpp)

target_include_directories(oysyn_search PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CURL_INCLUDE_DIR}
)

target_link_libraries(oysyn_search PRIVATE
    ${CURL_LIBRARY}
    httplib::httplib
    nlohmann_json::nlohmann_json
    pugixml::pugixml
    pthread
)
