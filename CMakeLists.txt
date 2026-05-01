cmake_minimum_required(VERSION 3.10)
project(AutoDocEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Crow
FetchContent_Declare(
    crow
    GIT_REPOSITORY https://github.com/CrowCPP/Crow.git
    GIT_TAG master
)

# Asio
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
)

FetchContent_MakeAvailable(crow asio)

add_executable(auto_doc_engine main.cpp)

target_include_directories(auto_doc_engine PRIVATE
    ${crow_SOURCE_DIR}/include
    ${asio_SOURCE_DIR}/asio/include
)

if(TARGET Crow::Crow)
    target_link_libraries(auto_doc_engine PRIVATE Crow::Crow)
elseif(TARGET Crow)
    target_link_libraries(auto_doc_engine PRIVATE Crow)
endif()

# Linux needs pthread
if(UNIX)
    target_link_libraries(auto_doc_engine PRIVATE pthread)
endif()

# Windows needs Winsock
if(WIN32)
    target_link_libraries(auto_doc_engine PRIVATE ws2_32 mswsock)
endif()