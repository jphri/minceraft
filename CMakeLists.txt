cmake_minimum_required(VERSION 3.1)
project(cubegame)

add_subdirectory(lib/stb)
add_subdirectory(lib/linmath)
add_subdirectory(lib/noise1234)

set(THREADS_PREFER_PTHREAD_FLAG ON)

find_library(GLFW glfw REQUIRED)
find_library(GLEW GLEW REQUIRED)
find_library(OPENGL OpenGL REQUIRED)
find_package(Threads REQUIRED)

file(GLOB src "src/*.c")
add_custom_target(COPY_SHADERS ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory
	${PROJECT_SOURCE_DIR}/shaders
	${PROJECT_BINARY_DIR}/shaders
	COMMENT "Copy shaders..."
)
add_custom_target(COPY_TEXTURES ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory
	${PROJECT_SOURCE_DIR}/textures
	${PROJECT_BINARY_DIR}/textures
	COMMENT "Copy textures..."
)

set(LIBRARIES noise1234 linmath stb ${GLFW} ${GLEW} ${OPENGL})
if(UNIX)
	set(LIBRARIES ${LIBRARIES} m)
endif()

add_executable(minceraft ${src})
add_dependencies(minceraft COPY_SHADERS COPY_TEXTURES)
target_link_libraries(minceraft PRIVATE ${LIBRARIES} Threads::Threads)

target_compile_options(minceraft PRIVATE -O3 -Wall -Wextra -pedantic -Wno-implicit-fallthrough)
target_link_options(minceraft PRIVATE -O3 -Wall -Wextra -pedantic)
