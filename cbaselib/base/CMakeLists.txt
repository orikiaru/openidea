cmake_minimum_required(VERSION 2.6)

set(LIBNAME base)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC 	
	src/oi_lock.cpp 
	src/oi_bytes.cpp 
	src/oi_thread.cpp 
	src/oi_bytes_stream.cpp
	src/oi_config.cpp
	src/oi_file.cpp
	src/oi_registry.cpp
	)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc ../net/inc)
target_include_directories(${LIBRARY_M} PUBLIC inc ../net/inc)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)
