cmake_minimum_required(VERSION 3.20)
project(Macross30Recompiled
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Guest longjmp deliberately unwinds through the runtime's extern-C dispatcher.
# /EHsc assumes such calls cannot throw and removes the Lua catch/cleanup path.
# Keep synchronous C++ unwinding while allowing C-linkage calls to propagate it.
if(MSVC)
    string(REPLACE "/EHsc" "/EHs /EHc-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# Opt-in native field diagnosis using the SDK's existing timing counters.
# The definition belongs ONLY to this SDK translation unit: no lifted PPU/SPU
# sources or shared headers change ABI, and toggling it rebuilds just this
# object plus the library/executable link. Configure with
# MACROSS_SPU_STATIC_ENTRIES=audio to preserve the existing SPU selection.
option(MACROSS_LIVE_PERF_PROFILE "Profile live RSX draw/compile/upload/fence costs" OFF)
if(MACROSS_LIVE_PERF_PROFILE)
    set_property(SOURCE "${PS3RECOMP_DIR}/libs/video/rsx_live_draw.c"
        TARGET_DIRECTORY ps3recomp_runtime
        # Legacy title-specific SPU benchmark hooks have no provider in this
        # SDK tree. Exclude those hooks explicitly, retaining actual RSX costs.
        APPEND PROPERTY COMPILE_DEFINITIONS YZ_PERF_PROFILE=1 YZ_RSX_PROFILE_ONLY=1)
endif()

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(PS3RECOMP_SPU_LIFTER "${CMAKE_CURRENT_SOURCE_DIR}/spu_lifter_wrapper.py")
# ps3recomp_generate_spu shares its exact supported generator closure with the
# content cache; unrelated trace/report tools must not invalidate SPU lifts.
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    set(MACROSS_SPU_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_spu" CACHE PATH
        "Directory for generated native SPU sources")
    ps3recomp_generate_spu("${MACROSS_SPU_OUTPUT_DIR}" ${PORT_SPU_INPUTS})
    # Edge MLAA kernels return early while the GPU runs the post-process
    # (spu_postfx_skip.c); idempotent, re-applied after every generation.
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/build/patch_spu_postfx_skip.py"
                            "${MACROSS_SPU_OUTPUT_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/spu_notify/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_graphics/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_notify/register.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_postfx_skip.c")
list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/spu_notify_native.c")
list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom1/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/anim/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom2/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom3/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom6/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom7/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom4/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom5/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/jobcrt14/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/jobcrt14_b/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/jobcrt14_c/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/jobcrt14_d/spu_recomp.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/register.c")
add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    font_probe.cpp
    lua_probe.cpp
    lua_unwind.cpp
    l10n_bridge.cpp
    pam_decoder.cpp
    pam_audio_decoder.cpp
    main_cpu_probe.cpp
    geom_cpu_probe.cpp
    geom_c5b0_capture.cpp
    realtime_step.cpp
    sail_playback.cpp
    movie_subtitle_timeline.cpp
    movie_subtitle_renderer.cpp
    battle_voice_catalog.cpp
    battle_voice_subtitles.cpp
    battle_voice_acceptance_probe.cpp
    spu_queue_backoff.c
    ppu_wait_backoff.cpp
    native_zlib.cpp
    native_zlib_core.c
    postfx_host.cpp
    postfx_host_core.c
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)
option(MACROSS_LINK_MAP "Emit native linker map for CPU profiling" OFF)
set_source_files_properties(spu_queue_backoff.c PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime")
set_source_files_properties(native_zlib_core.c PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/libs/codec")
if(MSVC AND MACROSS_LINK_MAP)
    target_link_options(${PROJECT_NAME} PRIVATE
        "/MAP:${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map")
endif()

# Raw post-processing plugins live outside the embedded SPU ELF text ranges.
# Their guarded lifts are opt-in at runtime; keep their flags local so no PPU
# translation unit is rebuilt when testing the bridge.
option(MACROSS_BUILD_POSTTASK_NATIVE "Build guarded native raw posttask plugins" ON)
if(MACROSS_BUILD_POSTTASK_NATIVE)
    set(MACROSS_POSTTASK_NATIVE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/guard.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family0.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family1.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family2.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family4.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family6.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family8.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family9.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family10.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family11.c")
    target_sources(${PROJECT_NAME} PRIVATE ${MACROSS_POSTTASK_NATIVE_SOURCES})
    set_source_files_properties(${MACROSS_POSTTASK_NATIVE_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        set_source_files_properties(${MACROSS_POSTTASK_NATIVE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    elseif(MSVC)
        set_source_files_properties(${MACROSS_POSTTASK_NATIVE_SOURCES} PROPERTIES COMPILE_OPTIONS "/O2")
    endif()
    set(MACROSS_POSTTASK_ISA "scalar" CACHE STRING "Native posttask ISA: scalar, sse41, avx2")
    set_property(CACHE MACROSS_POSTTASK_ISA PROPERTY STRINGS scalar sse41 avx2)
    if(NOT MACROSS_POSTTASK_ISA MATCHES "^(scalar|sse41|avx2)$")
        message(FATAL_ERROR "Invalid MACROSS_POSTTASK_ISA")
    endif()
    # Validated scale1 LUT remains runtime OFF unless MACROSS_POSTTASK_F11_LUT=1.
    # No AVX2 candidate object enters scalar/SSE4.1 builds.
    if(MACROSS_POSTTASK_ISA STREQUAL "avx2")
        set(MACROSS_POSTTASK_F11_LUT_SOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family11_lut.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/family11_lut_outer.c")
        target_sources(${PROJECT_NAME} PRIVATE ${MACROSS_POSTTASK_F11_LUT_SOURCES})
        set_source_files_properties(${MACROSS_POSTTASK_F11_LUT_SOURCES} PROPERTIES
            INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
            COMPILE_OPTIONS "-O2;-mavx2")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/guard.c"
            APPEND PROPERTY COMPILE_DEFINITIONS MACROSS_POSTTASK_F11_LUT_BUILT=1)
    endif()
    if(NOT MACROSS_POSTTASK_ISA STREQUAL "scalar")
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            message(FATAL_ERROR "Posttask SIMD requires Clang or GNU")
        endif()
        set(MACROSS_POSTTASK_KERNELS ${MACROSS_POSTTASK_NATIVE_SOURCES})
        list(REMOVE_ITEM MACROSS_POSTTASK_KERNELS "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/guard.c")
        if(MACROSS_POSTTASK_ISA STREQUAL "avx2")
            set(MACROSS_POSTTASK_ISA_FLAG "-mavx2")
            set(MACROSS_POSTTASK_ISA_LEVEL 2)
        else()
            set(MACROSS_POSTTASK_ISA_FLAG "-msse4.1")
            set(MACROSS_POSTTASK_ISA_LEVEL 1)
        endif()
        set_property(SOURCE ${MACROSS_POSTTASK_KERNELS} APPEND PROPERTY COMPILE_OPTIONS "${MACROSS_POSTTASK_ISA_FLAG}")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/spu_posttask_native/guard.c"
            APPEND PROPERTY COMPILE_DEFINITIONS "MACROSS_POSTTASK_ISA_LEVEL=${MACROSS_POSTTASK_ISA_LEVEL}")
    endif()
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/spu/spu_channels.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_DEFINITIONS MACROSS_POSTTASK_NATIVE=1)
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu;${PS3RECOMP_DIR}/runtime"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
endif()

option(MACROSS_SPU_SSE41 "Use verified SSE4.1 byte shuffles in lifted SPU modules" OFF)
if(MACROSS_SPU_SSE41)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "SPU SSE4.1 requires Clang or GNU")
    endif()
    if(NOT SPU_SOURCES)
        message(FATAL_ERROR "Lifted SPU modules missing")
    endif()
    set_property(SOURCE ${SPU_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-msse4.1")
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS MACROSS_SPU_SSE41=1)
endif()

option(MACROSS_GAMEPLAY_AVX2 "Use exact AVX2 float helpers in captured gameplay jobs" OFF)
if(MACROSS_GAMEPLAY_AVX2)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Gameplay AVX2 requires Clang or GNU")
    endif()
    foreach(MACROSS_GAMEPLAY_KERNEL IN ITEMS geom1 geom2 geom3 geom4 geom5 geom6 geom7 anim)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/${MACROSS_GAMEPLAY_KERNEL}/spu_recomp.c"
            APPEND PROPERTY COMPILE_OPTIONS "-mavx2" "-ffp-contract=off")
    endforeach()
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS MACROSS_GAMEPLAY_AVX2=1)
endif()

option(MACROSS_GEOM_DEBUG_LINES "Emit optimized geom1 CodeView lines for host-IP diagnosis" OFF)
if(MACROSS_GEOM_DEBUG_LINES)
    if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "Geom CodeView diagnosis requires Windows Clang")
    endif()
    # Keep the optimized kernel and its inlining. Only this TU gains source
    # records; avoid rebuilding the very large unrelated PPU translation units.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/spu_gameplay/geom1/spu_recomp.c"
        APPEND PROPERTY COMPILE_OPTIONS "-gline-tables-only" "-gcodeview")
    target_link_options(${PROJECT_NAME} PRIVATE "/DEBUG:FULL"
        "/PDB:${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pdb")
endif()

option(MACROSS_RSX_DEBUG_LINES "Emit optimized CodeView lines for the RSX/renderer TUs (line-level host profiling)" OFF)
if(MACROSS_RSX_DEBUG_LINES)
    if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RSX CodeView diagnosis requires Windows Clang")
    endif()
    # Same as the geom switch: source records for the renderer thread's own
    # files only, optimization unchanged, no rebuild of the PPU units.
    set_property(SOURCE
        "${PS3RECOMP_DIR}/libs/video/cellGcmSys.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_live_draw.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_commands.c"
        "${PS3RECOMP_DIR}/libs/video/rsx_dispatch.c"
        TARGET_DIRECTORY ps3recomp_runtime
        APPEND PROPERTY COMPILE_OPTIONS "-gline-tables-only" "-gcodeview")
    target_link_options(${PROJECT_NAME} PRIVATE "/DEBUG:FULL"
        "/PDB:${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pdb")
endif()

option(MACROSS_SPU_FUNCTION_PROFILE "Build sampled inclusive SPU function timing" OFF)
if(MACROSS_SPU_FUNCTION_PROFILE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SPU profiling requires cleanup attribute support")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE spu_function_perf.c)
    set_source_files_properties(spu_function_perf.c PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime")
    foreach(MACROSS_PROFILE_SOURCE IN LISTS SPU_SOURCES)
        if(MACROSS_PROFILE_SOURCE MATCHES "spu_(2f512e1a72325554e198ffd3ae1e76a3b349caa24715ffdd8cf4b0873b32a3ff|b00089288811e0386af134b05f015fe4dd39173f00250001bf158290adbf67b8)/spu_recomp.c$")
            set_property(SOURCE "${MACROSS_PROFILE_SOURCE}" APPEND PROPERTY COMPILE_DEFINITIONS MACROSS_SPU_FUNCTION_PROFILE=1)
        endif()
    endforeach()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        bcrypt
        winmm
    )
    # cellPad's SDL2 controller backend (delay-loaded; see the SDK).
    if(PS3RECOMP_SDL2_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PS3RECOMP_SDL2_DLL}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#   - AVX2/FMA (clang-cl): fmaf() -- which the VMX vmaddfp/vnmsubfp helpers
#     call per lane -- becomes one vfmadd instead of a CRT software routine
#     (same single-rounding result). Contraction stays off, so no other
#     multiply/add pair is fused. MACROSS_PPU_NO_AVX2=ON drops it.
option(MACROSS_PPU_NO_AVX2 "Build lifted PPU sources without AVX2/FMA" OFF)
set(MACROSS_PPU_EXTRA_DEFS "" CACHE STRING "Extra compile options for lifted PPU sources (bisecting)")
if(MSVC)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT MACROSS_PPU_NO_AVX2)
        set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
            COMPILE_OPTIONS "/w;/bigobj;/arch:AVX2;/clang:-ffp-contract=off;${MACROSS_PPU_EXTRA_DEFS}")
    else()
        set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
            COMPILE_OPTIONS "/w;/bigobj")
    endif()
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# BEGIN MACROSS_PPU_O2_SELECTION
# Empty by default: an experiment may rebuild selected PPU units without
# changing the other large generated units or enabling relaxed FP semantics.
set(MACROSS_PPU_O2_SOURCES "" CACHE STRING
    "Semicolon-separated generated PPU basenames to optimize at O2 (e.g. ppu_recomp_013.cpp)")
set(_macross_ppu_o2_seen "")
foreach(_ppu_name IN LISTS MACROSS_PPU_O2_SOURCES)
    if(NOT _ppu_name MATCHES "^ppu_recomp_[0-9][0-9][0-9]\\.cpp$")
        message(FATAL_ERROR "MACROSS_PPU_O2_SOURCES requires numbered PPU basenames: ${_ppu_name}")
    endif()
    set(_ppu_source "${RECOMP_DIR}/${_ppu_name}")
    if(NOT EXISTS "${_ppu_source}" OR NOT _ppu_source IN_LIST RECOMP_SOURCES)
        message(FATAL_ERROR "Selected PPU source is not in RECOMP_SOURCES: ${_ppu_source}")
    endif()
    if(NOT _ppu_source IN_LIST _macross_ppu_o2_seen)
        # Source options follow target/configuration options, overriding O1.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set_property(SOURCE "${_ppu_source}" APPEND PROPERTY COMPILE_OPTIONS
                "-O2" "-ffp-contract=off")
        elseif(MSVC)
            set_property(SOURCE "${_ppu_source}" APPEND PROPERTY COMPILE_OPTIONS "/O2")
        else()
            set_property(SOURCE "${_ppu_source}" APPEND PROPERTY COMPILE_OPTIONS
                "-O2" "-ffp-contract=off")
        endif()
        list(APPEND _macross_ppu_o2_seen "${_ppu_source}")
        message(STATUS "Selected PPU O2: ${_ppu_name}")
    endif()
endforeach()
# END MACROSS_PPU_O2_SELECTION

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
