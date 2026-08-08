# The specification's own build sequence (spec 20.8), plus the determinism
# guarantee of spec 15.8. Driven by ctest through cmake -P so it runs the same
# way on every platform.
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
file(MAKE_DIRECTORY "${WORK}/src")
file(GLOB SPEC_SOURCES "${SPEC_DIR}/*.manta")
file(COPY ${SPEC_SOURCES} DESTINATION "${WORK}/src")
file(GLOB SOURCES "${WORK}/src/*.manta")

# --- compile ---------------------------------------------------------------
run_manta(compile -o "${WORK}/build/" ${SOURCES})

# --- compiling twice is byte-identical (spec 15.8) -------------------------
run_manta(compile -o "${WORK}/build2/" ${SOURCES})
file(GLOB OBJECTS RELATIVE "${WORK}/build" "${WORK}/build/*.mantaO")
foreach(object ${OBJECTS})
    file(SHA256 "${WORK}/build/${object}" a)
    file(SHA256 "${WORK}/build2/${object}" b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "compile is not deterministic: ${object}")
    endif()
endforeach()

# --- link ------------------------------------------------------------------
# ERC is skipped here: spec 20.7 is an excerpt of a board, and several of its
# nets are genuinely undriven. The conformance suite covers ERC directly.
#
# The sources are not yet annotated, so this first link demotes E-UNANNOTATED.
# That is the bootstrap: 'manta annotate' reads its assignments from a netlist,
# so a netlist has to exist before annotation is possible at all.
run_manta(link --top power-and-signal -L "${WORK}/build" --no-erc -Wno-unannotated
          --bom "${WORK}/bom.csv" --map "${WORK}/map.txt" -o "${WORK}/board.mantaNets")
run_manta(link --top power-and-signal -L "${WORK}/build" --no-erc -Wno-unannotated
          -o "${WORK}/board2.mantaNets")
file(SHA256 "${WORK}/board.mantaNets" a)
file(SHA256 "${WORK}/board2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "link is not deterministic")
endif()

# --- fmt: formatting must not change what the design means -----------------
file(MAKE_DIRECTORY "${WORK}/fmt")
foreach(source ${SOURCES})
    get_filename_component(name "${source}" NAME)
    execute_process(COMMAND "${MANTA}" fmt --stdout "${source}"
                    OUTPUT_FILE "${WORK}/fmt/${name}" RESULT_VARIABLE code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "fmt failed on ${source}")
    endif()
    # ...and formatting is idempotent.
    execute_process(COMMAND "${MANTA}" fmt --stdout "${WORK}/fmt/${name}"
                    OUTPUT_FILE "${WORK}/fmt/${name}.again" RESULT_VARIABLE code)
    file(SHA256 "${WORK}/fmt/${name}" c)
    file(SHA256 "${WORK}/fmt/${name}.again" d)
    if(NOT c STREQUAL d)
        message(FATAL_ERROR "fmt is not idempotent on ${name}")
    endif()
    file(REMOVE "${WORK}/fmt/${name}.again")
endforeach()

file(GLOB FORMATTED "${WORK}/fmt/*.manta")
run_manta(compile -o "${WORK}/fmtbuild/" ${FORMATTED})
run_manta(link --top power-and-signal -L "${WORK}/fmtbuild" --no-erc -Wno-unannotated
          -o "${WORK}/fmt.mantaNets")
file(SHA256 "${WORK}/fmt.mantaNets" e)
if(NOT a STREQUAL e)
    message(FATAL_ERROR "formatting changed the netlist")
endif()

# --- annotate --------------------------------------------------------------
run_manta(annotate -n "${WORK}/board.mantaNets" --dry-run ${SOURCES})
run_manta(annotate -n "${WORK}/board.mantaNets" ${SOURCES})
# Annotated source still compiles and links (spec 13.1: a build leaves the tree
# clean and nothing in the netlist depends on a designator existing).
run_manta(compile -o "${WORK}/annbuild/" ${SOURCES})
# No -Wno-unannotated this time: every instance now carries a designator, which
# is the whole point of having annotated. A range designator such as "L%[1:2]"
# hands its members out one per copy (spec 13.3), so those resolve too.
run_manta(link --top power-and-signal -L "${WORK}/annbuild" --no-erc
          -o "${WORK}/ann.mantaNets")
# Annotating again must change nothing (spec 13.6: designators are stable).
file(READ "${WORK}/src/board.manta" before)
run_manta(annotate -n "${WORK}/ann.mantaNets" ${SOURCES})
file(READ "${WORK}/src/board.manta" after)
if(NOT before STREQUAL after)
    message(FATAL_ERROR "annotate renumbered an already-assigned designator")
endif()

# --- export ----------------------------------------------------------------
foreach(format kicad altium orcad allegro)
    run_manta(export --format ${format} -o "${WORK}/board.${format}"
              --constraints "${WORK}/constraints.json" "${WORK}/board.mantaNets")
    file(SIZE "${WORK}/board.${format}" size)
    if(size LESS 100)
        message(FATAL_ERROR "export ${format} produced almost nothing")
    endif()

    # Spec 15.8, the same rule every other stage is held to. A UUID or a map
    # leaking iteration order into the output would show up here and nowhere
    # else, because export is the one stage whose input is already a file.
    run_manta(export --format ${format} -o "${WORK}/board2.${format}"
              "${WORK}/board.mantaNets")
    file(SHA256 "${WORK}/board.${format}" a)
    file(SHA256 "${WORK}/board2.${format}" b)
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "export ${format} is not deterministic")
    endif()
endforeach()

# Every component reaches the netlist with its pins attached. A designator is
# unique only within its block, so two instances of one block both hold an 'R1';
# if the netlist names them both 'R1' a reader cannot tell them apart and the
# second copy silently arrives with no connections at all.
file(READ "${WORK}/board.kicad" kicad_net)
file(STRINGS "${WORK}/board.kicad" comp_refs REGEX "\\(comp \\(ref ")
foreach(line ${comp_refs})
    string(REGEX REPLACE ".*\\(comp \\(ref \"([^\"]*)\".*" "\\1" ref "${line}")
    # A literal search, not MATCHES: an un-annotated designator contains '?',
    # which a regex would read as an operator and match the wrong thing.
    string(FIND "${kicad_net}" "(node (ref \"${ref}\")" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "component '${ref}' is in the netlist with no connections")
    endif()
endforeach()

# A footprint with no library nickname will not place in KiCad, so exporting one
# is a warning; '-Werror' is how a project refuses to ship a netlist that cannot
# be laid out. tests/spec uses bare package names, so this must fire.
execute_process(COMMAND "${MANTA}" export --format kicad -Werror
                        -o "${WORK}/board.werror.net" "${WORK}/board.mantaNets"
                ERROR_VARIABLE fp_err RESULT_VARIABLE fp_code)
if(fp_code EQUAL 0)
    message(FATAL_ERROR "an unqualified footprint did not warn")
endif()
if(NOT fp_err MATCHES "W-FOOTPRINT")
    message(FATAL_ERROR "expected W-FOOTPRINT, got: ${fp_err}")
endif()

# ...and giving it a library silences it.
file(WRITE "${WORK}/fp.map" "# every package tests/spec uses\n")
foreach(fp QFP-STM32-32 TSSOP-14 SOD-323 HDR-1x4 BGA-96 TP-1MM R-0603 C-0603)
    file(APPEND "${WORK}/fp.map" "${fp}  Test_Library:${fp}\n")
endforeach()
run_manta(export --format kicad --footprint-map "${WORK}/fp.map" --footprint-lib Fallback
          -Werror -o "${WORK}/board.mapped.net" "${WORK}/board.mantaNets")
file(STRINGS "${WORK}/board.mapped.net" bare REGEX "\\(footprint \"[^:\"]*\"\\)")
if(bare)
    message(FATAL_ERROR "a footprint reached KiCad with no library: ${bare}")
endif()

# A map file that names no library defeats its own purpose, so it is refused.
file(WRITE "${WORK}/bad.map" "R-0603  R_0603_1608Metric\n")
execute_process(COMMAND "${MANTA}" export --format kicad --footprint-map "${WORK}/bad.map"
                        -o "${WORK}/board.bad.net" "${WORK}/board.mantaNets"
                ERROR_VARIABLE map_err RESULT_VARIABLE map_code)
if(map_code EQUAL 0)
    message(FATAL_ERROR "a map entry with no library was accepted")
endif()

# --- the end-of-content marker (spec 2.8) ----------------------------------
# 'manta fmt' rewrites whole files from the AST, so without deliberate care it
# would delete everything after the marker. This is the check that it does not.
set(DATASHEET "${WORK}/src/datasheet.manta")
file(READ "${DATASHEET}" datasheet_before)
execute_process(COMMAND "${MANTA}" fmt --stdout "${DATASHEET}"
                OUTPUT_FILE "${WORK}/datasheet.fmt" RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "fmt failed on a file with an end-of-content marker")
endif()
file(READ "${WORK}/datasheet.fmt" datasheet_after)

string(FIND "${datasheet_before}" "\n---\n" before_at)
string(FIND "${datasheet_after}" "\n---\n" after_at)
if(before_at EQUAL -1 OR after_at EQUAL -1)
    message(FATAL_ERROR "the end-of-content marker did not survive formatting")
endif()
string(SUBSTRING "${datasheet_before}" ${before_at} -1 tail_before)
string(SUBSTRING "${datasheet_after}" ${after_at} -1 tail_after)
if(NOT tail_before STREQUAL tail_after)
    message(FATAL_ERROR "formatting altered the text after the end-of-content marker")
endif()

message(STATUS "pipeline: compile, link, fmt, annotate and export all verified")
