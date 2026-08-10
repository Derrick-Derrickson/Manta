# Unit tests. Each file is a standalone binary registered with ctest.
file(GLOB MANTA_UNIT_TESTS CONFIGURE_DEPENDS unit/test_*.cpp)

foreach(src ${MANTA_UNIT_TESTS})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE manta_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE MANTA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    manta_set_warnings(${name})
    add_test(NAME unit.${name} COMMAND ${name})
endforeach()

# The specification's own build sequence (spec 20.8) plus the determinism
# guarantee of spec 15.8, end to end through the real binary.
add_test(NAME pipeline
         COMMAND ${CMAKE_COMMAND}
                 -DMANTA=$<TARGET_FILE:manta>
                 -DSPEC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/spec
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/pipeline-work
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/pipeline.cmake)

# Spec 15.4: ".mantaO and .mantaNets are JSON, validated against a published
# JSON Schema". Runs after `pipeline`, over the artifacts it produced.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME schema
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/validate_schema.py
                     ${CMAKE_SOURCE_DIR}/schema
                     ${CMAKE_CURRENT_BINARY_DIR}/pipeline-work)
    set_tests_properties(schema PROPERTIES DEPENDS pipeline)
endif()

# The schematic layout engine: rooms, idioms, port flags and render
# determinism, through the real binary. The debug builds additionally prove,
# via the renderer's internal assert, that no two placed symbols overlap.
add_test(NAME render
         COMMAND ${CMAKE_COMMAND}
                 -DMANTA=$<TARGET_FILE:manta>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/render-work
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/render.cmake)

# A complete design that must pass every check with nothing to report -- the
# other half of the conformance argument.
add_test(NAME example
         COMMAND ${CMAKE_COMMAND}
                 -DMANTA=$<TARGET_FILE:manta>
                 -DEXAMPLE_DIR=${CMAKE_SOURCE_DIR}/examples/blinky
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/example-work
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/example.cmake)
