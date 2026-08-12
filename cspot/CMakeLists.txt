cmake_minimum_required(VERSION 3.10)

project(cspot)

# Configurable options
set(CSPOT_EXTERNAL_BELL "" CACHE STRING "External bell library target name, optional")

# CMake options
set(CMAKE_CXX_STANDARD 20)

if(MSVC)
	add_compile_definitions(NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
	add_definitions(/wd4068 /wd4244 /wd4018 /wd4101 /wd4102 /wd4142 /wd4996)
endif()

# Main library sources
file(GLOB SOURCES "src/*.cpp" "src/*.c")

# Use externally specified bell library or the submodule
if(CSPOT_EXTERNAL_BELL)
    list(APPEND EXTRA_LIBS ${CSPOT_EXTERNAL_BELL})
else()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/bell)
    list(APPEND EXTRA_LIBS bell)
endif()

# Build protobuf code
# Use pre-generated files when available (avoids nanopb Python protobuf version conflicts).
# The pre-generated directory lives at <project_root>/proto_generated relative to this file.
set(_PREGENERATED "${CMAKE_CURRENT_SOURCE_DIR}/../proto_generated")
if(EXISTS "${_PREGENERATED}")
    file(GLOB PROTO_SRCS "${_PREGENERATED}/protobuf/*.c")
    file(GLOB PROTO_HDRS "${_PREGENERATED}/protobuf/*.h")
    set(_PROTO_INCLUDE_DIR "${_PREGENERATED}")
    message(STATUS "cspot: using pre-generated protobuf files from ${_PREGENERATED}")
else()
    set(NANOPB_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}")
    file(GLOB PROTOS protobuf/*.proto)
    nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTOS})
    set(_PROTO_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

add_custom_target(generate_proto_sources DEPENDS ${PROTO_SRCS} ${PROTO_HDRS})
set_source_files_properties(${PROTO_SRCS} ${PROTO_HDRS} PROPERTIES GENERATED TRUE)

add_library(cspot STATIC ${SOURCES} ${PROTO_SRCS})
# PUBLIC to propagate includes from bell to cspot dependents
target_compile_definitions(bell PUBLIC PB_ENABLE_MALLOC)
target_compile_definitions(bell PUBLIC PB_FIELD_32BIT)
target_link_libraries(cspot PUBLIC ${EXTRA_LIBS})
target_include_directories(cspot PUBLIC "include" ${_PROTO_INCLUDE_DIR} ${NANOPB_INCLUDE_DIRS} ${EXTRA_INCLUDES})
