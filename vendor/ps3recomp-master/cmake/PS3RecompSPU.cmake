# Generate all discovered SPU ELF programs from explicitly supplied input files.
# The helper runs at configure time so the source list belongs to this exact
# generation. Input/tool changes trigger reconfiguration before compilation.
function(ps3recomp_generate_spu output_dir)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(command "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/build_spu_workloads.py"
        --lifted "${output_dir}" --out "${output_dir}/spu_workloads.c" --constructor)
    if(PS3RECOMP_SPU_LIFTER)
        list(APPEND command --lifter "${PS3RECOMP_SPU_LIFTER}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS3RECOMP_SPU_LIFTER}")
    endif()
    foreach(input IN LISTS ARGN)
        get_filename_component(input "${input}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND command --input "${input}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    endforeach()
    set(selected_lifter "${PS3RECOMP_DIR}/tools/spu_lifter.py")
    if(PS3RECOMP_SPU_LIFTER)
        set(selected_lifter "${PS3RECOMP_SPU_LIFTER}")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${PS3RECOMP_DIR}/tools/spu_generator_dependencies.py" --lifter "${selected_lifter}"
        RESULT_VARIABLE deps_result OUTPUT_VARIABLE generator_deps ERROR_VARIABLE deps_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT deps_result EQUAL 0)
        message(FATAL_ERROR "Cannot determine SPU generator dependencies: ${deps_error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${generator_deps})
    execute_process(COMMAND ${command} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "SPU generation incomplete. Inspect ${output_dir}/inventory.json.\n${output}\n${error}")
    endif()
    include("${output_dir}/sources.cmake")
    set(SPU_GENERATED_SOURCES "${SPU_GENERATED_SOURCES}" PARENT_SCOPE)
endfunction()
