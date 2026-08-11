# Revision 1.5, spec 11.3 and 11.10: '&RAIL' and '&EDGE' end to end through the
# real binary. Both directives are display hints: they must reach the netlist --
# 'RAIL' in a net's directives object with an empty value, 'edge' on the
# component that wrote it and on no other -- and a netlist carrying them must
# still be read back by the tools that consume one.
#
# Expects: MANTA (path to the binary), SPEC_DIR (tests/spec), WORK (scratch dir).

function(run_manta)
    set(cmd "${MANTA}")
    foreach(arg ${ARGN})
        list(APPEND cmd "${arg}")
    endforeach()
    execute_process(COMMAND ${cmd} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "manta ${ARGN}\nexit ${code}\n${out}${err}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# ---------------------------------------------------------------------------
# Source -> object -> netlist, twice, byte-identical (spec 15.8)
# ---------------------------------------------------------------------------
# 'manta link' reads .mantaO, not source, so a netlist that carries both
# directives is itself the proof that the object format carried them; the
# byte-identical re-compile is the determinism guarantee on the new forms.
file(GLOB SPEC_SOURCES "${SPEC_DIR}/*.manta")
run_manta(compile -o "${WORK}/build/" ${SPEC_SOURCES})
run_manta(compile -o "${WORK}/build2/" ${SPEC_SOURCES})
file(GLOB OBJECTS RELATIVE "${WORK}/build" "${WORK}/build/*.mantaO")
foreach(object ${OBJECTS})
    file(SHA256 "${WORK}/build/${object}" a)
    file(SHA256 "${WORK}/build2/${object}" b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "compile is not deterministic: ${object}")
    endif()
endforeach()

run_manta(link --top hints-demo -L "${WORK}/build" --no-erc -o "${WORK}/hints.mantaNets")
run_manta(link --top hints-demo -L "${WORK}/build" --no-erc -o "${WORK}/hints2.mantaNets")
file(SHA256 "${WORK}/hints.mantaNets" a)
file(SHA256 "${WORK}/hints2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "linking a design with '&RAIL' and '&EDGE' is not deterministic")
endif()

file(READ "${WORK}/hints.mantaNets" net)

# ---------------------------------------------------------------------------
# The component 'edge' key: on J1, and on nothing else
# ---------------------------------------------------------------------------
string(JSON comp_count LENGTH "${net}" components)
math(EXPR comp_last "${comp_count} - 1")
set(saw_j1 FALSE)
foreach(i RANGE ${comp_last})
    string(JSON d GET "${net}" components ${i} designator)
    string(JSON edge ERROR_VARIABLE edge_missing GET "${net}" components ${i} edge)
    if(d STREQUAL "J1")
        set(saw_j1 TRUE)
        if(NOT edge_missing STREQUAL "NOTFOUND" OR NOT edge STREQUAL "LEFT")
            message(FATAL_ERROR "J1 wrote '&EDGE=LEFT' but carries edge '${edge}'")
        endif()
    elseif(edge_missing STREQUAL "NOTFOUND")
        # 'edge' is emitted only when written (spec 15.4), so a component that
        # never wrote '&EDGE' must not carry the key at all.
        message(FATAL_ERROR "'${d}' wrote no '&EDGE' but carries edge '${edge}'")
    endif()
endforeach()
if(NOT saw_j1)
    message(FATAL_ERROR "the connector J1 did not reach the netlist")
endif()

# ---------------------------------------------------------------------------
# The net 'RAIL' directive: value-less, so an empty string (spec 15.4)
# ---------------------------------------------------------------------------
string(JSON net_count LENGTH "${net}" nets)
math(EXPR net_last "${net_count} - 1")
set(saw_rail FALSE)
foreach(i RANGE ${net_last})
    string(JSON n GET "${net}" nets ${i} name)
    if(NOT n STREQUAL "HSYS-PROT")
        continue()
    endif()
    set(saw_rail TRUE)
    string(JSON rail ERROR_VARIABLE rail_missing GET "${net}" nets ${i} directives RAIL)
    if(NOT rail_missing STREQUAL "NOTFOUND" OR NOT rail STREQUAL "")
        message(FATAL_ERROR "HSYS-PROT does not carry RAIL with an empty value: "
                            "'${rail}' (${rail_missing})")
    endif()
    # ...and the value-carrying directive on the same statement is untouched.
    string(JSON current GET "${net}" nets ${i} directives CURRENT)
    if(NOT current STREQUAL "2A")
        message(FATAL_ERROR "HSYS-PROT lost its '&CURRENT=2A' beside '&RAIL': '${current}'")
    endif()
endforeach()
if(NOT saw_rail)
    message(FATAL_ERROR "the rail net HSYS-PROT did not reach the netlist")
endif()

# ---------------------------------------------------------------------------
# A netlist carrying the new keys reads back
# ---------------------------------------------------------------------------
# 'manta export' is the one stage whose input is a finished .mantaNets, so it is
# the reader's proof: a consumer written against 1.4 keys must not choke on a
# component that carries 'edge'.
run_manta(export --format kicad -o "${WORK}/hints.net" "${WORK}/hints.mantaNets")
file(STRINGS "${WORK}/hints.net" j1_ref REGEX "J1")
if(NOT j1_ref)
    message(FATAL_ERROR "the exported netlist lost the connector J1")
endif()

message(STATUS "hints: '&RAIL' and '&EDGE' reach the netlist and read back")
