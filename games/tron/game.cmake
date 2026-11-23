# Tron: Evolution (Xbox 360) - the PS5 title.
#
#   tools/build.sh            (runs ../PS5_ReXGlueTemplate/tools/build.sh with GAME=games/tron)
#
# The code is ReXGlue's translation of the player's own default.xex
# (recomp/generated/default, made by tools/codegen.sh). No game code or assets
# are kept in the repository: the player copies their disc's files into the
# title folder's gamedata/.

get_filename_component(TRON_ROOT ${TITLE_DIR}/../.. ABSOLUTE)
set(TRON_GENERATED_DIR ${TRON_ROOT}/recomp/generated/default)
if(NOT EXISTS ${TRON_GENERATED_DIR}/sources.cmake OR
   NOT EXISTS ${TRON_GENERATED_DIR}/tronevolution_init.h)
	message(FATAL_ERROR "No generated code at ${TRON_GENERATED_DIR}: run tools/codegen.sh "
		"with your own default.xex in recomp/assets/")
endif()

set(TITLE_RESULTS_FILE /app0/title-results.txt)
set(TITLE_ARGS --log_level=info)

add_library(tron_game OBJECT
	${TITLE_DIR}/src/tron_ps5_app.cpp
	${TITLE_DIR}/src/diag_hooks.cpp
	${REX_APP_SOURCE})
target_include_directories(tron_game PRIVATE
	${TITLE_DIR}/src ${TRON_GENERATED_DIR}
	${REXGLUE_SRC}/thirdparty/vulkan-headers/include
	${REXGLUE_SRC}/thirdparty/vulkan-memory-allocator/include
	${REXGLUE_SRC}/thirdparty/glslang
	${REXGLUE_SRC}/thirdparty/renderdoc)
rex_ps5_settings(tron_game)
target_compile_options(tron_game PRIVATE -fno-char8_t)
rex_ps5_link_objects(tron_game)

rex_ps5_module(tron_recomp GENERATED ${TRON_GENERATED_DIR}
	INCLUDES ${TITLE_DIR}/src ${REXGLUE_SRC}/thirdparty/vulkan-headers/include)

message(STATUS "Tron Evolution: game code from ${TRON_GENERATED_DIR}")
