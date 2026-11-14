# 基础库，不含 modules。
# 本仓库的 CMakeLists 和外部工程都 include 这个文件。
# include 不另开目录作用域，编译选项、头文件目录、BASE_LIBS 留在调用方。
if(OI_FRAMEWORK_INCLUDED)
	return()
endif()
set(OI_FRAMEWORK_INCLUDED TRUE)

# 按本文件所在目录定位，不能用 CMAKE_SOURCE_DIR。被业务仓库 include 时源码根是业务仓库。
get_filename_component(OI_FRAMEWORK_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(BASE_LIB_PATH "${OI_FRAMEWORK_DIR}/cbaselib")
set(PROTO_TOOL_DIR "${BASE_LIB_PATH}/proto/proto_tool")

if(MSVC)
	add_compile_options(/W3)
	add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++20>)
	add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
	add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
else()
	set(_CXX_OPTION_ -ggdb -g -Wall -std=gnu++20
		-Werror -Wsuggest-override -Wno-unused-function
		-Wno-unused-but-set-variable
		-Wno-literal-suffix
		-Wno-format-security
		-Wno-overloaded-virtual
	)
	# GCC 10 的协程还要单独打开。GCC 11 起 -std=gnu++20 已默认带上。
	if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
		list(APPEND _CXX_OPTION_ -fcoroutines)
	endif()
	# Clang 不认识 GCC 的 -Wno-literal-suffix。
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		list(INSERT _CXX_OPTION_ 0 -Wno-unknown-warning-option)
	endif()
	# 只加到 C++。这些选项进了 C 文件后，gcc 会直接拒绝编译 libuv。
	foreach(_opt IN LISTS _CXX_OPTION_)
		add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${_opt}>")
	endforeach()
endif()

set(BASE_M_LIBS db_m_static timer_m_static logger_m_static redis_m_static net_m_static proto_m_static base_m_static net_m_static timer_m_static hiredis_static)
set(BASE_LIBS db_static timer_static logger_static redis_static net_static proto_static base_static net_static timer_static hiredis_static)
option(OI_ENABLE_MYSQL "Build the MySQL database driver" OFF)

# libuv 只编静态库，跟工程一起编译
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${BASE_LIB_PATH}/3rd/libuv" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/3rd/hiredis" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/3rd/jsoncpp" EXCLUDE_FROM_ALL)
if(MSVC AND TARGET uv_a)
	target_compile_options(uv_a PRIVATE /W0)
endif()

# 通用头文件目录。放在 libuv 后面，避免 proto/inc/rpc.h 盖住 Windows 的 rpc.h。
# 业务自己的 modules/include 由调用方加上，这里不带。
include_directories(
	${BASE_LIB_PATH}/include
	${BASE_LIB_PATH}/base/inc
	${BASE_LIB_PATH}/net/inc
	${BASE_LIB_PATH}/timer/inc
	${BASE_LIB_PATH}/logger/inc
	${BASE_LIB_PATH}/redis/inc
	${BASE_LIB_PATH}/db/inc
	${BASE_LIB_PATH}/http/inc
	${BASE_LIB_PATH}/proto/inc
	${BASE_LIB_PATH}/port
)
include_directories(SYSTEM
	${BASE_LIB_PATH}/3rd/cpptoml
	${BASE_LIB_PATH}/3rd/libuv/include
	${BASE_LIB_PATH}/3rd/hiredis
	${BASE_LIB_PATH}/3rd/jsoncpp/include
)

if(MSVC)
	include_directories(${BASE_LIB_PATH}/port/win)
	add_compile_options("/FI${BASE_LIB_PATH}/port/win/msvc_prefix.h")
	# 三方头走 SYSTEM 目录，按外部头处理，不再报它们自己的警告。
	add_compile_options(/external:W0)
endif()

find_program(PYTHON_BIN NAMES python3 python)
if(NOT PYTHON_BIN)
	message(FATAL_ERROR "Python 3 is required to generate protocols")
endif()
set(OI_FRAMEWORK_PROTO_FILES "${BASE_LIB_PATH}/proto/protocol/framework.proto")
file(GLOB OI_PROTO_TOOL_SOURCES "${PROTO_TOOL_DIR}/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OI_FRAMEWORK_PROTO_FILES} ${OI_PROTO_TOOL_SOURCES})
execute_process(
	COMMAND "${PYTHON_BIN}" "${PROTO_TOOL_DIR}/main.py" --data-only
		"${BASE_LIB_PATH}/base/inc/framework_protocol" -- ${OI_FRAMEWORK_PROTO_FILES}
	RESULT_VARIABLE OI_FRAMEWORK_PROTO_GEN
)
if(NOT OI_FRAMEWORK_PROTO_GEN EQUAL 0)
	message(FATAL_ERROR "Framework protocol generation failed: ${OI_FRAMEWORK_PROTO_GEN}")
endif()

# runtime_dir 由最外层 CMake 传入，框架不自己决定二进制放哪。logd 固定多线程，registry 固定单线程。
function(oi_add_framework_programs runtime_dir)
	if(NOT runtime_dir)
		message(FATAL_ERROR "oi_add_framework_programs requires the runtime output directory from the top-level CMakeLists")
	endif()
	add_executable(logd
		${BASE_LIB_PATH}/logd/src/main.cpp
		${BASE_LIB_PATH}/logd/src/log_server.cpp
		${BASE_LIB_PATH}/logd/src/log_server_session.cpp
		${BASE_LIB_PATH}/logd/src/log_manager.cpp
		${BASE_LIB_PATH}/logd/src/log_file_instance.cpp
	)
	target_include_directories(logd PRIVATE ${BASE_LIB_PATH}/logd/inc)
	if(MSVC)
		target_link_libraries(logd PUBLIC ${BASE_M_LIBS})
	else()
		target_link_libraries(logd PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(logd PUBLIC pthread)
	endif()

	add_executable(registry
		${BASE_LIB_PATH}/registry/src/main.cpp
		${BASE_LIB_PATH}/registry/src/registry_server.cpp
	)
	target_include_directories(registry PRIVATE ${BASE_LIB_PATH}/registry/src)
	target_compile_definitions(registry PRIVATE _SINGLE_THREAD_)
	if(MSVC)
		target_link_libraries(registry PUBLIC ${BASE_LIBS})
	else()
		target_link_libraries(registry PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
	endif()
	set_target_properties(logd registry PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${runtime_dir}")
endfunction()

add_subdirectory("${BASE_LIB_PATH}/base" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/net" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/proto" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/timer" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/logger" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/redis" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/http" EXCLUDE_FROM_ALL)
add_subdirectory("${BASE_LIB_PATH}/db" EXCLUDE_FROM_ALL)

# 按这次配置的系统写 .clangd。Visual Studio 不产 compile_commands，编译时再刷一份 Ninja 数据库。
function(oi_write_clangd_file clangd_dir db_dir)
	file(RELATIVE_PATH oi_rel "${clangd_dir}" "${db_dir}")
	string(REPLACE "\\" "/" oi_rel "${oi_rel}")
	set(oi_text "CompileFlags:\n  CompilationDatabase: ${oi_rel}\n")
	if(WIN32)
		string(APPEND oi_text "  Compiler: clang-cl\n")
	endif()
	string(APPEND oi_text "  Add:\n    - -std=c++20\n    - -ferror-limit=0\n")
	get_filename_component(oi_clangd_abs "${clangd_dir}" ABSOLUTE)
	get_filename_component(oi_framework_abs "${OI_FRAMEWORK_DIR}" ABSOLUTE)
	if(oi_clangd_abs STREQUAL oi_framework_abs)
		string(APPEND oi_text "    - -Icbaselib/3rd/cpp-httplib\n")
	endif()
	string(APPEND oi_text "Index:\n  Background: Build\n")
	file(WRITE "${clangd_dir}/.clangd" "${oi_text}")
endfunction()

if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
	set(OI_CLANGD_DB "${CMAKE_BINARY_DIR}")
	set(OI_CLANGD_REFRESH FALSE)
else()
	if(WIN32)
		set(OI_CLANGD_DB "${CMAKE_SOURCE_DIR}/build-clangd-win")
	else()
		set(OI_CLANGD_DB "${CMAKE_SOURCE_DIR}/build-clangd-linux")
	endif()
	set(OI_CLANGD_REFRESH TRUE)
endif()

oi_write_clangd_file("${CMAKE_SOURCE_DIR}" "${OI_CLANGD_DB}")
get_filename_component(OI_SOURCE_ABS "${CMAKE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(OI_FRAMEWORK_ABS "${OI_FRAMEWORK_DIR}" ABSOLUTE)
if(NOT OI_SOURCE_ABS STREQUAL OI_FRAMEWORK_ABS)
	oi_write_clangd_file("${OI_FRAMEWORK_DIR}" "${OI_CLANGD_DB}")
endif()

if(OI_CLANGD_REFRESH AND NOT TARGET oi_clangd_db)
	get_filename_component(oi_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
	if(NOT OI_NINJA OR OI_NINJA MATCHES "NOTFOUND")
		unset(OI_NINJA CACHE)
	endif()
	find_program(OI_NINJA NAMES ninja ninja.exe
		HINTS "${oi_cmake_bin}/../../Ninja" "${oi_cmake_bin}"
	)
	if(OI_NINJA)
		# 和主构建共用 deploy 下的产物，开关要一致，否则会把 oi_test_hooks.txt 刷回默认值。
		set(oi_clangd_options)
		foreach(oi_option OI_IPC_SINGLE_THREAD OI_ENABLE_TEST_HOOKS OI_ENABLE_MYSQL OI_MYSQL_INCLUDE_DIR OI_MYSQL_LIBRARY)
			if(DEFINED ${oi_option})
				list(APPEND oi_clangd_options "-D${oi_option}=${${oi_option}}")
			endif()
		endforeach()
		add_custom_target(oi_clangd_db ALL
			COMMAND "${CMAKE_COMMAND}" -G Ninja
				-S "${CMAKE_SOURCE_DIR}"
				-B "${OI_CLANGD_DB}"
				-DCMAKE_BUILD_TYPE=Release
				-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
				-DCMAKE_MAKE_PROGRAM=${OI_NINJA}
				-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
				-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
				${oi_clangd_options}
			COMMENT "Regenerate clangd compile_commands"
			VERBATIM
		)
	else()
		message(WARNING "ninja not found; .clangd was written but compile_commands will not refresh on build")
	endif()
endif()
