cmake_minimum_required(VERSION 2.6)

#logclient
set(LIBNAME logger)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC 	
	src/oi_log_client.cpp
	src/oi_log_client_session.cpp
	src/oi_log.cpp
	)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc ../net/inc ../base/inc)
target_include_directories(${LIBRARY_M} PUBLIC inc ../net/inc ../base/inc)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)
