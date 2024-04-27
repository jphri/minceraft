cmake_minimum_required(VERSION 3.1)
project(cubegame)

add_subdirectory(lib/stb)
add_subdirectory(lib/linmath)

find_library(GLFW glfw REQUIRED)
find_library(GLEW GLEW REQUIRED)
find_library(OPENGL OpenGL REQUIRED)

file(GLOB src "src/*.c")

set(LIBRARIES linmath stb ${GLFW} ${GLEW} ${OPENGL})
if(UNIX)
	set(LIBRARIES ${LIBRARIES} m)
endif()

add_executable(minceraft ${src})
target_link_libraries(minceraft PRIVATE ${LIBRARIES})
