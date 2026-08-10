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
# Nothing may still be carrying a '?'. An unassigned name reaches the netlist,
# the BOM and the layout tool, so "it linked" is not the same as "it annotated".
file(STRINGS "${WORK}/ann.mantaNets" leftover REGEX "\"designator\": \"[^\"]*[?]")
if(leftover)
    message(FATAL_ERROR "an unassigned designator survived annotation: ${leftover}")
endif()

# --- block instances -------------------------------------------------------
# A block instance is not a component and has no Component::designator, but its
# label names a level of the hierarchy and lands in the path of everything
# beneath it. tests/spec declares a block and never instantiates one, which is
# how an un-annotated block once reached the netlist unreported.
set(BLOCKSRC "${WORK}/blocks.manta")
file(WRITE "${BLOCKSRC}" "\
part BR-1k { @~footprint = R-0603; #value = 1kR; 1 = A &CASUAL; 2 = B &CASUAL; 3 = SHIELD; };

block clamp {
    >TAP;
    TAP = .{R9~BR-1k}. = CGND;
};

block leg {
    >IN;
    IN = .{R1~BR-1k: SHIELD = ?;}. = BGND;
    {CL1~clamp: TAP = BGND;};
};

block blocktop {
    BGND &TYPE=GROUND &STUB;
    BPWR>>;
    {U9~BR-1k: A = BDRIVE[0]; B = BDRIVE[1]; SHIELD = ?;};
    BDRIVE[0:1] = [[{BLK%[1:2]~leg}IN]];
    BDRIVE[0] == BPWR;
    BDRIVE[1] == BPWR;
    {U8~BR-1k: A = BSENSE; B = BFEED; SHIELD = ?;};
    BSENSE = {BLK3~clamp}TAP;
    TAP{BLK4~clamp} = BFEED;
};
")
run_manta(compile -o "${WORK}/blockbuild/" "${BLOCKSRC}")

# A range designator is the *annotated* form (spec 13.3): one token carrying N
# designators, handed out one per copy. Reading it as unassigned would report an
# annotated design as un-annotated and put a '?' in the netlist.
run_manta(link --top blocktop -L "${WORK}/blockbuild" --no-erc
          -o "${WORK}/blocks.mantaNets")
foreach(want "\"BLK1\"" "\"BLK2\"")
    file(STRINGS "${WORK}/blocks.mantaNets" hit REGEX "${want}")
    if(NOT hit)
        message(FATAL_ERROR "a range designator on a block did not resolve to ${want}")
    endif()
endforeach()
file(STRINGS "${WORK}/blocks.mantaNets" bad REGEX "BLK[?]")
if(bad)
    message(FATAL_ERROR "a range-designated block was treated as unassigned: ${bad}")
endif()

# --- block instance records (revision 1.3) ---------------------------------
# The netlist carries one record per child block instance -- 'leg' twice, and
# the 'clamp' nested inside each copy -- so a renderer can rebuild the
# hierarchy. In a CMake regex '.' matches newline, so a pattern can span the
# pretty-printed JSON.
file(READ "${WORK}/blocks.mantaNets" blocks_net)

string(REGEX MATCHALL "\"block\": \"leg\"" legs "${blocks_net}")
list(LENGTH legs leg_count)
string(REGEX MATCHALL "\"block\": \"clamp\"" clamps "${blocks_net}")
list(LENGTH clamps clamp_count)
# Four clamps: one nested in each 'leg' copy, plus BLK3 and BLK4 at the top.
if(NOT leg_count EQUAL 2 OR NOT clamp_count EQUAL 4)
    message(FATAL_ERROR "expected 2 'leg' and 4 'clamp' block records, "
                        "got ${leg_count} and ${clamp_count}")
endif()

# A nested instance carries the full path from the top.
if(NOT blocks_net MATCHES "\"BLK1\",[\r\n ]+\"CL1\"")
    message(FATAL_ERROR "the nested clamp does not carry its full instance path")
endif()

# --- a chain terminal on a block instance binds its port ---------------------
# Two pins share one net in a .mantaNets held in the named variable. Walked
# with string(JSON) rather than a regex: a pin's logical name may contain the
# ']' or '}' any textual bound would lean on.
function(assert_same_net netsvar ref_a pin_a ref_b pin_b)
    string(JSON net_count LENGTH "${${netsvar}}" nets)
    math(EXPR net_last "${net_count} - 1")
    foreach(i RANGE ${net_last})
        string(JSON pins GET "${${netsvar}}" nets ${i} pins)
        string(JSON pin_count LENGTH "${pins}")
        if(pin_count EQUAL 0)
            continue()
        endif()
        set(has_a FALSE)
        set(has_b FALSE)
        math(EXPR pin_last "${pin_count} - 1")
        foreach(p RANGE ${pin_last})
            string(JSON d GET "${pins}" ${p} designator)
            string(JSON n GET "${pins}" ${p} pin)
            if(d STREQUAL ref_a AND n STREQUAL pin_a)
                set(has_a TRUE)
            endif()
            if(d STREQUAL ref_b AND n STREQUAL pin_b)
                set(has_b TRUE)
            endif()
        endforeach()
        if(has_a AND has_b)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "${ref_a}.${pin_a} and ${ref_b}.${pin_b} do not share a net")
endfunction()

# No two nets may share a name: KiCad and friends merge nets BY NAME on import,
# so a duplicate would short two distinct nets on the real board.
function(assert_unique_net_names netsvar)
    string(JSON net_count LENGTH "${${netsvar}}" nets)
    math(EXPR net_last "${net_count} - 1")
    set(names "")
    foreach(i RANGE ${net_last})
        string(JSON name GET "${${netsvar}}" nets ${i} name)
        list(FIND names "${name}" at)
        if(NOT at EQUAL -1)
            message(FATAL_ERROR "two nets are both named '${name}'")
        endif()
        list(APPEND names "${name}")
    endforeach()
endfunction()

# "BDRIVE[0:1] = [[{BLK%[1:2]~leg}IN]]" must put the parent-side pin and the
# child-side pin on ONE net: U9.1 with BLK1's R1.1, U9.2 with BLK2's R1.1. The
# terminal faces away from the connector, which is exactly the spelling that
# once united nothing and left every child port floating.
assert_same_net(blocks_net U9 1 BLK1_R1 1)
assert_same_net(blocks_net U9 2 BLK2_R1 1)
# ...and the scalar spellings, exit-terminal and entry-terminal:
# "BSENSE = {BLK3~clamp}TAP" and "TAP{BLK4~clamp} = BFEED".
assert_same_net(blocks_net U8 1 BLK3_R9 1)
assert_same_net(blocks_net U8 2 BLK4_R9 1)

# Block-local nets flatten as components do: 'leg' spells a BGND of its own, so
# each instance's copy is emitted under its instance path while the top-level
# BGND keeps its bare name -- four scopes spell one word, four distinct nets.
assert_unique_net_names(blocks_net)
foreach(want "\"name\": \"BGND\"" "\"name\": \"BLK1.BGND\"" "\"name\": \"BLK2.BGND\""
        "\"name\": \"BLK1_CL1.CGND\"")
    string(FIND "${blocks_net}" "${want}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "expected a net named ${want}")
    endif()
endforeach()

# Every declared port resolved to a real net: nothing in this design leaves a
# port dangling, so a '-1' means the resolution went wrong.
string(REGEX MATCHALL "\"name\": \"IN\",[\r\n ]+\"direction\": \"in\",[\r\n ]+\"net\": [0-9]+"
       in_ports "${blocks_net}")
list(LENGTH in_ports in_port_count)
if(NOT in_port_count EQUAL 2)
    message(FATAL_ERROR "expected 2 resolved 'IN' ports, got ${in_port_count}")
endif()
if(blocks_net MATCHES "\"net\": -1")
    message(FATAL_ERROR "a block port resolved to no net")
endif()

# The local spelling of each instance's nets survives, which is what lets a
# renderer label a child page with 'BGND' rather than the parent-flat name.
foreach(local BGND CGND TAP)
    if(NOT blocks_net MATCHES "\"localNets\":[^]]*\"name\": \"${local}\"")
        message(FATAL_ERROR "'${local}' is missing from a block's localNets")
    endif()
endforeach()

# Per-component pin lists, in declaration order. R1's SHIELD is unbound with
# '&NET=?', so it sits on no net at all and the component entry is the only
# place it survives; R9's is merely unconnected and gets a one-pin net.
if(NOT blocks_net MATCHES "\"pin\": \"1\",[\r\n ]+\"name\": \"A\"")
    message(FATAL_ERROR "a component entry carries no declared pin list")
endif()
if(NOT blocks_net MATCHES "\"pin\": \"2\",[\r\n ]+\"name\": \"B\"[^]]*\"pin\": \"3\",[\r\n ]+\"name\": \"SHIELD\"")
    message(FATAL_ERROR "the declared pin list is not in declaration order")
endif()
if(blocks_net MATCHES "R1.SHIELD")
    message(FATAL_ERROR "an unbound pin surfaced as a net")
endif()

# The same design left un-annotated must fail the link.
file(READ "${BLOCKSRC}" blocks_text)
string(REPLACE "BLK%[1:2]" "BLK?" blocks_text "${blocks_text}")
file(WRITE "${WORK}/blocks-unassigned.manta" "${blocks_text}")
run_manta(compile -o "${WORK}/blockbuild2/" "${WORK}/blocks-unassigned.manta")

execute_process(COMMAND "${MANTA}" link --top blocktop -L "${WORK}/blockbuild2" --no-erc
                        -o "${WORK}/blocks2.mantaNets"
                ERROR_VARIABLE blk_err RESULT_VARIABLE blk_code)
if(blk_code EQUAL 0)
    message(FATAL_ERROR "an un-annotated block instance linked without complaint")
endif()
if(NOT blk_err MATCHES "E-UNANNOTATED")
    message(FATAL_ERROR "expected E-UNANNOTATED for a block, got: ${blk_err}")
endif()

# ...and the bootstrap still works, because 'manta annotate' reads a netlist and
# there has to be a way to produce the first one (spec 13.1).
run_manta(link --top blocktop -L "${WORK}/blockbuild2" --no-erc -Wno-unannotated
          -o "${WORK}/blocks2.mantaNets")

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

# --- render ----------------------------------------------------------------
# The HTML schematic is inside the determinism guarantee (spec 15.8); --pdf is
# not exercised here because it needs a browser on PATH.
run_manta(render -o "${WORK}/board.html" "${WORK}/ann.mantaNets")
run_manta(render -o "${WORK}/board2.html" "${WORK}/ann.mantaNets")
file(SHA256 "${WORK}/board.html" a)
file(SHA256 "${WORK}/board2.html" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "render is not deterministic")
endif()

# The interactivity contract: nets carry data-net, symbols data-c, and each
# sheet is a section a sidebar anchor can reach.
file(STRINGS "${WORK}/board.html" rendered_nets REGEX "data-net=")
if(NOT rendered_nets)
    message(FATAL_ERROR "the rendered schematic has no data-net elements")
endif()
file(STRINGS "${WORK}/board.html" rendered_syms REGEX "data-c=")
if(NOT rendered_syms)
    message(FATAL_ERROR "the rendered schematic has no data-c symbols")
endif()
file(STRINGS "${WORK}/board.html" rendered_pages REGEX "id=\"page-")
if(NOT rendered_pages)
    message(FATAL_ERROR "the rendered schematic has no page sections")
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

# --- connectors, cables and mating (spec 12A) -------------------------------
# A cable is its own deliverable: it links on its own, with its own netlist and
# its own BOM, and a board's netlist never absorbs one.
set(CABLEDIR "${CMAKE_CURRENT_LIST_DIR}/cable")
run_manta(compile -o "${WORK}/cable/" "${CABLEDIR}/card.manta" "${CABLEDIR}/loom.manta")

run_manta(link --top jumper-8way -L "${WORK}/cable" -Werror
          -o "${WORK}/loom.mantaNets" --bom "${WORK}/loom.csv")

# Determinism, as every other stage is held to.
run_manta(link --top jumper-8way -L "${WORK}/cable" -Werror -o "${WORK}/loom2.mantaNets")
file(SHA256 "${WORK}/loom.mantaNets" a)
file(SHA256 "${WORK}/loom2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "linking a cable is not deterministic")
endif()

# A cable renders too: a loom has no footprints and its parts are wires and
# crimps, which must not trip the schematic renderer.
run_manta(render -o "${WORK}/loom.html" "${WORK}/loom.mantaNets")

# A loom's BOM carries its wires and crimps, which is the whole reason a cable
# is a first-class thing rather than a comment.
file(STRINGS "${WORK}/loom.csv" wires REGEX ",wire,")
file(STRINGS "${WORK}/loom.csv" crimps REGEX ",crimp,")
if(NOT wires OR NOT crimps)
    message(FATAL_ERROR "a cable BOM lists no wires or no crimps")
endif()

# The board alone: the mating is checked, the cable is not emitted. E-01 is
# quieted as the bad-* fixtures already do: U2's TX and J1's outbound TXD pin
# are one net -- the card re-declares the signal's direction at its boundary,
# which the on-board driver count reads as a second driver.
set(QUIET -Wno-W-04 -Wno-W-09 -Wno-E-01 -Wno-E-02)
run_manta(link --top sensor-card -L "${WORK}/cable" ${QUIET} -o "${WORK}/card.mantaNets")

# '--assembly' writes the loom beside the board and never merges the two.
file(REMOVE "${WORK}/jumper-8way.mantaNets")
execute_process(COMMAND "${MANTA}" link --top sensor-card -L "${WORK}/cable" ${QUIET}
                        --assembly -o "${WORK}/card2.mantaNets" --bom "${WORK}/card.csv"
                WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE asm_code)
if(NOT asm_code EQUAL 0)
    message(FATAL_ERROR "--assembly failed")
endif()
if(NOT EXISTS "${WORK}/jumper-8way.mantaNets")
    message(FATAL_ERROR "--assembly wrote no netlist for the mated cable")
endif()
file(SHA256 "${WORK}/card.mantaNets" a)
file(SHA256 "${WORK}/card2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "--assembly changed the board's own netlist")
endif()

# Each check must be shown to fire. A check that cannot fail is worth nothing,
# and every one of these was written only after watching it fail.
function(expect_mating_error name top code)
    file(REMOVE_RECURSE "${WORK}/bad-${name}")
    execute_process(COMMAND "${MANTA}" compile -o "${WORK}/bad-${name}/"
                            "${CABLEDIR}/bad-${name}.manta" "${CABLEDIR}/loom.manta"
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND "${MANTA}" link --top ${top} -L "${WORK}/bad-${name}"
                            -Wno-W-01 -Wno-W-04 -Wno-W-09 -Wno-E-01 -Wno-E-02
                            -Wno-E-24 -Wno-E-27 -Wno-E-28 -o "${WORK}/bad-${name}.mantaNets"
                    ERROR_VARIABLE err RESULT_VARIABLE code_out)
    if(code_out EQUAL 0)
        message(FATAL_ERROR "${code} did not fire on bad-${name}.manta")
    endif()
    if(NOT err MATCHES "${code}")
        message(FATAL_ERROR "expected ${code} on bad-${name}.manta, got: ${err}")
    endif()
endfunction()

expect_mating_error(contents bad-loom    "E-44")
expect_mating_error(fit      bd          "E-45")
expect_mating_error(pins     bd          "E-46")
expect_mating_error(drivers  sensor-card "E-47")
expect_mating_error(power    sensor-card "E-48")
