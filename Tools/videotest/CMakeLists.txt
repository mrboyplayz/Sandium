cmake_minimum_required(VERSION 3.20)
project(videotest CXX)
add_executable(videotest main.cpp)
target_link_libraries(videotest mfplat mfreadwrite mfuuid propsys)
