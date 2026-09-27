cmake_minimum_required(VERSION 3.21)

project("Sandium")

add_subdirectory("Sandium")
if (WIN32)
	add_subdirectory("SandiumLauncher")
endif ()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tools/poser/CMakeLists.txt")
	add_subdirectory("Tools/poser")
endif ()
