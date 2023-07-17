cmake_minimum_required(VERSION 2.6)

set(LIBNAME net)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC 	
	src/net_socket.cpp
	src/net_helper.cpp

	src/poll.cpp

	src/session_manager.cpp
	src/session.cpp 
	src/net_session.cpp 

	src/channel.cpp 
	src/accept_channel.cpp
	src/connect_channel.cpp
	src/net_channel.cpp 
	src/tcp_channel.cpp
	src/ssl_channel.cpp

	)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc ../net/inc)
target_include_directories(${LIBRARY_M} PUBLIC inc ../net/inc)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)