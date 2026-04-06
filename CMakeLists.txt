cmake_minimum_required(VERSION 3.16)
project(p2p-editor CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG        v5.0.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ftxui)

find_package(Threads REQUIRED)

file(GLOB P2P_SOURCES src/*.cpp)
if(P2P_SOURCES)
  add_executable(p2p-editor ${P2P_SOURCES})
  target_include_directories(p2p-editor PRIVATE include)
  target_link_libraries(p2p-editor PRIVATE
    ftxui::screen
    ftxui::dom
    ftxui::component
    Threads::Threads
  )
endif()

add_executable(tests
  tests/test_main.cpp
  tests/test_rga.cpp
  tests/test_serializer.cpp
  tests/test_peer_socket.cpp
  src/rga.cpp
  src/serializer.cpp
  src/peer_socket.cpp
)

target_include_directories(tests PRIVATE include)

target_link_libraries(tests PRIVATE
  Threads::Threads
)

option(ENABLE_ASAN "Build tests with AddressSanitizer" OFF)
option(ENABLE_TSAN "Build tests with ThreadSanitizer" OFF)

if(ENABLE_ASAN)
  target_compile_options(tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(tests PRIVATE -fsanitize=address)
endif()

if(ENABLE_TSAN)
  target_compile_options(tests PRIVATE -fsanitize=thread)
  target_link_options(tests PRIVATE -fsanitize=thread)
endif()
