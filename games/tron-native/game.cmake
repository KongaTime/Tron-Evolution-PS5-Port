# Tron: Evolution (Xbox 360) - the PS5 title with the native renderer.
#
#   tools/build-native.sh
#
# Same game code as games/tron, but the Xbox 360 D3D library's draw, clear,
# resolve and swap calls are taken over by the native renderer from the
# Dante's Inferno PS5 port (native/src/renderer, DiligentCore on RADV). The
# Xenos command processor only runs for sync (fences, interrupts, pacing).

get_filename_component(TRON_ROOT ${TITLE_DIR}/../.. ABSOLUTE)
set(TRON_GENERATED_DIR ${TRON_ROOT}/recomp/generated/default)
set(TRON_NATIVE ${TRON_ROOT}/native)
if(NOT EXISTS ${TRON_GENERATED_DIR}/sources.cmake OR
   NOT EXISTS ${TRON_GENERATED_DIR}/tronevolution_init.h)
	message(FATAL_ERROR "No generated code at ${TRON_GENERATED_DIR}: run tools/codegen.sh "
		"with your own default.xex in recomp/assets/")
endif()

set(diligent_root "$ENV{TRON_DILIGENT_ROOT}")
set(diligent_build "${diligent_root}/diligent-ps5")
if(NOT EXISTS "${diligent_build}/Graphics/GraphicsEngineVulkan/libDiligent-GraphicsEngineVk-static.a")
	message(FATAL_ERROR "No DiligentCore PS5 build under TRON_DILIGENT_ROOT=${diligent_root}")
endif()

set(TITLE_RESULTS_FILE /app0/title-results.txt)
set(TITLE_ARGS --log_level=info)

set(renderer_dir ${TRON_NATIVE}/src/renderer)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(ps5_spirv "${CMAKE_CURRENT_BINARY_DIR}/ps5_native_spirv.h")
add_custom_command(OUTPUT "${ps5_spirv}"
	COMMAND "${Python3_EXECUTABLE}" "${TRON_NATIVE}/tools/compile_shaders.py"
		"${TRON_NATIVE}" "${GLSLANG_VALIDATOR}" "${ps5_spirv}"
	DEPENDS "${TRON_NATIVE}/tools/compile_shaders.py"
		"${renderer_dir}/vulkan_device.cpp" "${renderer_dir}/renderer.cpp"
		"${renderer_dir}/ui_overlay.cpp" "${renderer_dir}/smaa_pass.cpp"
		"${TRON_NATIVE}/thirdparty/smaa/SMAA.hlsl"
	VERBATIM)

add_library(tron_game OBJECT
	${TITLE_DIR}/src/tron_ps5_app.cpp
	${TITLE_DIR}/src/frame_rate.cpp
	${TITLE_DIR}/src/tron_toast.cpp
	${TITLE_DIR}/src/tron_sound.cpp
	${TITLE_DIR}/src/launcher/menu.cpp
	${TITLE_DIR}/src/launcher/iso_import.cpp
	${REX_APP_SOURCE}
	${renderer_dir}/renderer.cpp
	${renderer_dir}/draw_renderer.cpp
	${renderer_dir}/pipeline_storage.cpp
	${renderer_dir}/png_dump.cpp
	${renderer_dir}/button_glyphs.cpp
	${renderer_dir}/shader_cache.cpp
	${renderer_dir}/smaa_pass.cpp
	${renderer_dir}/texture_cache.cpp
	${renderer_dir}/mod_loader.cpp
	${renderer_dir}/thread_cpu.cpp
	${renderer_dir}/renderer_config.cpp
	${renderer_dir}/ui_overlay.cpp
	${renderer_dir}/vulkan_device.cpp
	${renderer_dir}/d3d_hooks.cpp
	${renderer_dir}/renderdoc_capture.cpp
	${renderer_dir}/state_verifier.cpp
	${renderer_dir}/guest_gpu_state.cpp
	${ps5_spirv})
include(${TRON_NATIVE}/cmake/EmbedGlyphIcons.cmake)
hells_gate_embed_glyph_icons(tron_game ${TRON_NATIVE}/assets/glyphs/kenney)
target_compile_definitions(tron_game PRIVATE TRON_NATIVE=1 HELLS_GATE_PS5_NATIVE_GAME=1
	PLATFORM_LINUX=1)
target_include_directories(tron_game PRIVATE
	${TITLE_DIR}/src ${TRON_GENERATED_DIR}
	${TRON_NATIVE} ${TRON_NATIVE}/src
	${CMAKE_CURRENT_BINARY_DIR}
	${ROOT}/src
	${REXGLUE_SRC}/thirdparty/xxHash
	"${diligent_root}/diligent-core"
	"${diligent_root}/diligent-core/ThirdParty/stb"
	${REXGLUE_SRC}/thirdparty/vulkan-headers/include
	${REXGLUE_SRC}/thirdparty/vulkan-memory-allocator/include
	${REXGLUE_SRC}/thirdparty/glslang
	${REXGLUE_SRC}/thirdparty/renderdoc)
rex_ps5_settings(tron_game)
target_compile_options(tron_game PRIVATE -fno-char8_t)
rex_ps5_link_objects(tron_game)

rex_ps5_module(tron_recomp GENERATED ${TRON_GENERATED_DIR}
	INCLUDES ${TITLE_DIR}/src ${REXGLUE_SRC}/thirdparty/vulkan-headers/include)

# The title template supplies Volk; Diligent's Vulkan calls use those symbols.
set(diligent_archives
	Graphics/GraphicsEngineVulkan/libDiligent-GraphicsEngineVk-static.a
	Graphics/GraphicsTools/libDiligent-GraphicsTools.a
	Graphics/GraphicsEngine/libDiligent-GraphicsEngine.a
	Graphics/GraphicsAccessories/libDiligent-GraphicsAccessories.a
	Graphics/GraphicsEngineNextGenBase/libDiligent-GraphicsEngineNextGenBase.a
	Graphics/ShaderTools/libDiligent-ShaderTools.a
	Common/libDiligent-Common.a
	Platforms/Linux/libDiligent-LinuxPlatform.a
	Platforms/Basic/libDiligent-BasicPlatform.a
	Primitives/libDiligent-Primitives.a
	ThirdParty/SPIRV-Cross/libspirv-cross-core.a)
list(TRANSFORM diligent_archives PREPEND "${diligent_build}/")
set_property(GLOBAL APPEND PROPERTY TITLE_LINK_INPUTS --start-group ${diligent_archives} --end-group)
set_property(GLOBAL APPEND PROPERTY TITLE_LINK_DEPENDS ${diligent_archives})

message(STATUS "Tron Evolution (native renderer): game code from ${TRON_GENERATED_DIR}")

# Profile-guided builds (from the God of War Ascension PS5 port):
#   TRON_PGO=gen  instruments the game code and the native renderer; the title
#                 writes /app0/pgo/tron.profraw every 30 s (src/pgo_rt.cpp).
#   TRON_PGO=use  builds with TRON_PGO_PROFDATA (llvm-profdata merge of the dumps).
set(TRON_PGO "$ENV{TRON_PGO}")
set(TRON_PGO_PROFDATA "${TRON_ROOT}/pgo/tron.profdata")
if(TRON_PGO STREQUAL "gen")
	foreach(t tron_recomp tron_game)
		target_compile_options(${t} PRIVATE -fprofile-instr-generate)
	endforeach()
	add_library(tron_pgo_rt OBJECT ${TITLE_DIR}/src/pgo_rt.cpp)
	rex_ps5_settings(tron_pgo_rt)
	rex_ps5_link_objects(tron_pgo_rt)
	# The Linux profile runtime names glibc's `stderr`; renamed in a copy of the
	# archive (pgo_rt.c defines tron_pgo_stderr).
	set(_prof "${CMAKE_BINARY_DIR}/libclang_rt.profile-tron.a")
	execute_process(COMMAND llvm-objcopy-21 --redefine-sym=stderr=tron_pgo_stderr
		/usr/lib/llvm-21/lib/clang/21/lib/linux/libclang_rt.profile-x86_64.a "${_prof}")
	set_property(GLOBAL APPEND PROPERTY TITLE_LINK_INPUTS -T "${TITLE_DIR}/pgo-sections.ld" --no-dependent-libraries --allow-multiple-definition "${_prof}")
	message(STATUS "Tron Evolution: PGO instrumented build")
elseif(TRON_PGO STREQUAL "use" AND EXISTS "${TRON_PGO_PROFDATA}")
	foreach(t tron_recomp tron_game)
		target_compile_options(${t} PRIVATE "-fprofile-instr-use=${TRON_PGO_PROFDATA}"
			-Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin)
	endforeach()
	message(STATUS "Tron Evolution: PGO optimized build (${TRON_PGO_PROFDATA})")
endif()
