cmake_minimum_required(VERSION 2.6)

set(LIBNAME redis)
set(LIBRARY ${LIBNAME}_static)
set(LIBRARY_M ${LIBNAME}_m_static)

set(LIBSRC
	src/redis_client.cpp
	src/redis_client_session.cpp
)

add_library(${LIBRARY} STATIC ${LIBSRC})
add_library(${LIBRARY_M} STATIC ${LIBSRC})

target_include_directories(${LIBRARY} PUBLIC inc)
target_include_directories(${LIBRARY_M} PUBLIC inc)
target_link_libraries(${LIBRARY} PUBLIC hiredis_static)
target_link_libraries(${LIBRARY_M} PUBLIC hiredis_static)

target_compile_options(${LIBRARY} PUBLIC ${_CXX_OPTION_})
target_compile_options(${LIBRARY_M} PUBLIC ${_CXX_OPTION_})

target_compile_definitions(${LIBRARY} PUBLIC _SINGLE_THREAD_)
