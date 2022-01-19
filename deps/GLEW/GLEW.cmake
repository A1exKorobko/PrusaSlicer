# We have to check for OpenGL to compile GLEW
set(OpenGL_GL_PREFERENCE "LEGACY") # to prevent a nasty warning by cmake
find_package(OpenGL QUIET REQUIRED)

prusaslicer_add_cmake_project(
  GLEW
  SOURCE_DIR  ${CMAKE_CURRENT_LIST_DIR}/glew
  DEPENDS ${LIBCXX_PKG}
  CMAKE_ARGS
        ${MSAN_CMAKE_ARGS}
)

if (MSVC)
    add_debug_dep(dep_GLEW)
endif ()
