cmake_minimum_required(VERSION 3.21)

project("Sandium")

add_subdirectory("Sandium")
if (WIN32)
	add_subdirectory("SandiumLauncher")
endif ()