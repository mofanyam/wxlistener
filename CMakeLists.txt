cmake_minimum_required(VERSION 2.8)
project(wxlistener)


set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 -Wall")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2 -Wall")


add_executable(wxlistener wxlistener.c)

target_link_libraries(wxlistener ev)