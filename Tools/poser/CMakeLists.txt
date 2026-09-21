cmake_minimum_required(VERSION 3.21)

add_executable(
    SandiumPoser WIN32
    main.cpp
)

target_link_libraries(SandiumPoser PRIVATE comctl32 opengl32 gdi32 user32 comdlg32)
