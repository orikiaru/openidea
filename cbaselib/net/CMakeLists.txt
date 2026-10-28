cmake_minimum_required(VERSION 2.6)

set(LIBNAME net)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC 	
	src/oi_socket.cpp
	src/oi_net_helper.cpp

	src/oi_poll.cpp

	src/oi_session_manager.cpp
	src/oi_session.cpp 
	src/oi_net_session.cpp 

	src/oi_channel.cpp 
	src/oi_accept_channel.cpp
	src/oi_connect_channel.cpp
	src/oi_net_channel.cpp 
	src/oi_tcp_channel.cpp
	src/oi_ssl_channel.cpp
	../port/oi_net_os.cpp

	)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc ../net/inc)
target_include_directories(${LIBRARY_M} PUBLIC inc ../net/inc)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)

target_link_libraries(${LIBRARY} PUBLIC uv_a)
target_link_libraries(${LIBRARY_M} PUBLIC uv_a)
if(WIN32)
    target_link_libraries(${LIBRARY} PUBLIC ws2_32)
    target_link_libraries(${LIBRARY_M} PUBLIC ws2_32)
endif()