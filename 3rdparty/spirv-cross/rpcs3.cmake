# SPIRV-Cross build target for the native Metal 4 backend.
#
# Vendored as plain files in 3rdparty/spirv-cross (RPCS3_VENDORING.md, on the
# model 06 used for metal-cpp).  This file is the block the parent adds to
# 3rdparty/CMakeLists.txt, next to the 3rdparty_metal block:
#
#     include(${CMAKE_CURRENT_SOURCE_DIR}/spirv-cross/rpcs3.cmake)
#
# It exists only when the Metal backend is built (Apple + arm64 + USE_METAL4),
# mirroring the 3rdparty_metal_impl / 3rdparty_metal INTERFACE pattern.

set(SPIRV_CROSS_TARGET 3rdparty_dummy_lib)
if(USE_METAL4)
	if(NOT APPLE)
		# The Metal block above already reported this; nothing to add.
	elseif(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
		# The Metal block above already reported this; nothing to add.
	else()
		set(SPIRV_CROSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spirv-cross")

		add_library(3rdparty_spirv_cross_impl STATIC
			"${SPIRV_CROSS_DIR}/spirv_cross.cpp"
			"${SPIRV_CROSS_DIR}/spirv_cfg.cpp"
			"${SPIRV_CROSS_DIR}/spirv_cross_parsed_ir.cpp"
			"${SPIRV_CROSS_DIR}/spirv_parser.cpp"
			"${SPIRV_CROSS_DIR}/spirv_glsl.cpp"
			"${SPIRV_CROSS_DIR}/spirv_msl.cpp")

		# SPIRV-Cross is third-party.  RPCS3 compiles with -Werror=*
		# (buildfiles/cmake/ConfigureCompiler.cmake:51-79) and SPIRV-Cross has
		# unannotated switch fall-throughs, so the inherited set is neutralised
		# for this target only.  target_compile_options is appended after the
		# directory options, so the later flags win.
		#
		# RPCS3 builds with -fno-exceptions (ConfigureCompiler.cmake:33), which
		# turns the inherited SPIRV_CROSS_THROW into a compile error.  The library
		# is therefore built with exceptions so a bad module reaches the caller as
		# a throw: MTLShader.cpp is the single consumer and the single catch
		# (Emu/CMakeLists.txt).
		target_compile_options(3rdparty_spirv_cross_impl PRIVATE -w -Wno-error -fexceptions)

		# SYSTEM: SPIRV-Cross is third-party, as with metal-cpp.
		target_include_directories(3rdparty_spirv_cross_impl SYSTEM PUBLIC
			"${SPIRV_CROSS_DIR}"
			"${SPIRV_CROSS_DIR}/include")

		add_library(3rdparty_spirv_cross INTERFACE)
		target_link_libraries(3rdparty_spirv_cross INTERFACE 3rdparty_spirv_cross_impl)
		set(SPIRV_CROSS_TARGET 3rdparty_spirv_cross)
	endif()
endif()
