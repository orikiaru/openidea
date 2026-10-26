cmake_minimum_required(VERSION 2.6)

set(LIBNAME base)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC 	
	src/spin_lock.cpp 
	src/bytes.cpp 
	src/threadpool.cpp 
	src/bytes_stream.cpp
	src/config.cpp
	src/timer_task.cpp
	src/file_helper.cpp
	)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc ../net/inc)
target_include_directories(${LIBRARY_M} PUBLIC inc ../net/inc)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)
