cmake_minimum_required(VERSION 2.6)

#logclient
set(LIBNAME logger)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC 	
	src/log_client.cpp
	src/log_parser.cpp
	src/log_protocol.cpp
	src/log_client_session.cpp
	src/logger.cpp
	)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc ../net/inc ../base/inc)
target_include_directories(${LIBRARY_M} PUBLIC inc ../net/inc ../base/inc)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)


#logserver只允许使用多线程
set(LIBNAME2 logd)
set(LIBRARY_M2 ${LIBNAME2}_m_static)

#logserver也会包含logclient,所以重复的target不需要包含
set(LIBSRC2
	src/log_client.cpp
	src/log_parser.cpp
	src/log_client_session.cpp
	src/logger.cpp
	)

add_library(${LIBRARY_M2} STATIC ${LIBSRC2})
target_include_directories(${LIBRARY_M2} PUBLIC inc ../net/inc ../base/inc)
target_compile_options(${LIBRARY_M2} PUBLIC ${_CXX_OPTION_})
target_compile_definitions(${LIBRARY_M2} PUBLIC _LOG_SERVER_)
