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
	src/oi_blocking_work_session.cpp
	src/oi_blocking_work_channel.cpp
	src/oi_net_session.cpp 

	src/oi_channel.cpp 
	src/oi_accept_channel.cpp
	src/oi_connect_channel.cpp
	src/oi_net_channel.cpp 
	src/oi_tcp_channel.cpp
	src/oi_udp_channel.cpp
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

# TLS is layered on NetSession; plain TCP targets do not link OpenSSL.
if(WIN32)
	find_package(OpenSSL 3.2 REQUIRED COMPONENTS SSL)
else()
	find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL)
endif()
foreach(tls_target IN ITEMS tls_static tls_m_static)
	add_library(${tls_target} STATIC src/oi_tls_session.cpp)
	target_include_directories(${tls_target} PUBLIC inc)
	target_compile_options(${tls_target} PRIVATE ${_CXX_OPTION_})
	target_link_libraries(${tls_target} PRIVATE OpenSSL::SSL)
	if(WIN32)
		target_link_libraries(${tls_target} PRIVATE crypt32)
	endif()
endforeach()
target_link_libraries(tls_static PUBLIC net_static)
target_link_libraries(tls_m_static PUBLIC net_m_static)
