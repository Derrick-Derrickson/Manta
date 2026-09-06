# The blinky example must pass every check with nothing to report.
#
# The conformance suite proves each diagnostic fires on a design that earns it;
# this proves none of them fires on a design that does not. Both directions
# matter: a checker that never fires is useless, and one that always fires is
# worse.
#
# Expects: MANTA, EXAMPLE_DIR, WORK.

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
file(GLOB EXAMPLE_SOURCES "${EXAMPLE_DIR}/*.manta")

# The checked-in examples are canonical, so the formatter has nothing to do.
run_manta(fmt --check ${EXAMPLE_SOURCES})

set(RULES "${EXAMPLE_DIR}/blinky.mantaRules")

run_manta(compile -o "${WORK}/build/" ${EXAMPLE_SOURCES})

# The design compiles with no rules file at all: the '#' fields it carries are
# ordinary manta, so rules are pure checking and never a build dependency.

# -Werror, no --no-erc, no -Wno-, and the project's own rules loaded: every
# rule in section 16 runs, every user rule runs, every warning is fatal, and
# every instance carries a designator.
run_manta(check --top blinky -L "${WORK}/build" --rules "${RULES}" -Werror)

run_manta(link --top blinky -L "${WORK}/build" --rules "${RULES}" -Werror
          --bom "${WORK}/bom.csv" -o "${WORK}/blinky.mantaNets")

foreach(format kicad altium orcad allegro)
    run_manta(export --format ${format} -o "${WORK}/blinky.${format}"
              "${WORK}/blinky.mantaNets")
endforeach()

# The example ships a footprint map, so its KiCad netlist is one a board can
# actually be laid out from: '-Werror' means every footprint has to name a
# library, or the export fails rather than producing a file KiCad will reject.
set(FPMAP "${EXAMPLE_DIR}/blinky.fpmap")
run_manta(export --format kicad --footprint-map "${FPMAP}" -Werror
          -o "${WORK}/blinky.net" "${WORK}/blinky.mantaNets")

file(STRINGS "${WORK}/blinky.net" bare REGEX "\\(footprint \"[^:\"]*\"\\)")
if(bare)
    message(FATAL_ERROR "a footprint reached KiCad with no library: ${bare}")
endif()

# Every component is connected. Two copies of the 'indicator' block each hold an
# 'R1', so this is where a netlist that cannot tell them apart shows up.
file(READ "${WORK}/blinky.net" blinky_net)
file(STRINGS "${WORK}/blinky.net" comp_refs REGEX "\\(comp \\(ref ")
list(LENGTH comp_refs comp_count)
foreach(line ${comp_refs})
    string(REGEX REPLACE ".*\\(comp \\(ref \"([^\"]*)\".*" "\\1" ref "${line}")
    string(FIND "${blinky_net}" "(node (ref \"${ref}\")" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "component '${ref}' is in the netlist with no connections")
    endif()
endforeach()

# If KiCad's footprint libraries are installed, resolve every footprint the way
# Pcbnew does -- '<Library>.pretty/<Footprint>.kicad_mod' -- and check that each
# pin the netlist names exists as a pad. A footprint that resolves but whose
# pads are lettered would place the part and connect none of it.
set(KICAD_FOOTPRINTS "/usr/share/kicad/footprints")
if(IS_DIRECTORY "${KICAD_FOOTPRINTS}")
    file(STRINGS "${WORK}/blinky.net" fp_lines REGEX "\\(footprint \"")
    list(LENGTH fp_lines fp_count)
    if(NOT fp_count EQUAL comp_count)
        message(FATAL_ERROR "${comp_count} components but ${fp_count} footprints")
    endif()

    # The component list and the footprint list are emitted one after the other
    # in the same order, so index i of one belongs to index i of the other.
    set(pads_checked 0)
    math(EXPR last "${comp_count} - 1")
    foreach(i RANGE ${last})
        list(GET comp_refs ${i} comp_line)
        list(GET fp_lines ${i} fp_line)
        string(REGEX REPLACE ".*\\(comp \\(ref \"([^\"]*)\".*" "\\1" ref "${comp_line}")
        string(REGEX REPLACE ".*\\(footprint \"([^\"]*)\".*" "\\1" fp "${fp_line}")

        string(REPLACE ":" ";" parts "${fp}")
        list(GET parts 0 lib)
        list(GET parts 1 name)
        set(module "${KICAD_FOOTPRINTS}/${lib}.pretty/${name}.kicad_mod")
        if(NOT EXISTS "${module}")
            message(FATAL_ERROR "KiCad has no footprint '${fp}' for ${ref}")
        endif()
        file(READ "${module}" module_text)

        # Every pin this component uses has to exist as a pad. A footprint can
        # resolve and still be the wrong one: a real USB-C receptacle has pads
        # named A1 and B1, so a part declaring pins 1..4 would place and connect
        # nothing at all.
        file(STRINGS "${WORK}/blinky.net" node_lines REGEX "\\(node \\(ref \"")
        foreach(node ${node_lines})
            string(REGEX REPLACE ".*\\(node \\(ref \"([^\"]*)\"\\) \\(pin \"([^\"]*)\".*"
                   "\\1;\\2" pair "${node}")
            list(GET pair 0 node_ref)
            list(GET pair 1 node_pin)
            if(node_ref STREQUAL ref)
                string(FIND "${module_text}" "(pad \"${node_pin}\"" at)
                if(at EQUAL -1)
                    message(FATAL_ERROR
                            "${ref} uses pin ${node_pin}, which '${fp}' does not have")
                endif()
                math(EXPR pads_checked "${pads_checked} + 1")
            endif()
        endforeach()
    endforeach()
    message(STATUS "example: ${comp_count} footprint(s) and ${pads_checked} pad(s) "
                   "resolved against ${KICAD_FOOTPRINTS}")
else()
    message(STATUS "example: KiCad footprint libraries not installed, resolution not checked")
endif()

# The netlist records the two 'indicator' instances as block records with their
# ports resolved (revision 1.3), and U1's '.NC = ?' pin -- which sits on no net
# and so appears nowhere in the nets -- survives in U1's declared pin list.
file(READ "${WORK}/blinky.mantaNets" blinky_nets)
string(REGEX MATCHALL "\"block\": \"indicator\"" indicators "${blinky_nets}")
list(LENGTH indicators indicator_count)
if(NOT indicator_count EQUAL 2)
    message(FATAL_ERROR "expected 2 'indicator' block records, got ${indicator_count}")
endif()
if(NOT blinky_nets MATCHES "\"name\": \"DRIVE\",[\r\n ]+\"direction\": \"in\",[\r\n ]+\"net\": [0-9]+")
    message(FATAL_ERROR "the indicator's DRIVE port did not resolve to a net")
endif()
if(NOT blinky_nets MATCHES "\"pin\": \"4\",[\r\n ]+\"name\": \"NC\",[\r\n ]+\"type\": \"NC\"")
    message(FATAL_ERROR "U1's NC pin is missing from its declared pin list")
endif()

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

# The LEDs are actually driven: "LED-DRIVE[0:1] = [[{BLK%[1:2]~indicator}.DRIVE]]"
# must put U2's GPIO pin (PA3 is pin 9, PA4 pin 10) and the indicator's series
# resistor on ONE net, per channel. This is the cross-boundary membership no
# netlist ever had while the terminal-style port binding united nothing.
assert_same_net(blinky_nets U2 9 BLK1_R1 1)
assert_same_net(blinky_nets U2 10 BLK2_R1 1)

# No two nets may share a name: KiCad and friends merge nets BY NAME on import,
# so a duplicate would short the two LED channels on the real board.
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
assert_unique_net_names(blinky_nets)

# Each indicator's anode net is block-local, so it flattens under its instance
# path -- while the block record still carries the local spelling 'LED-ANODE'.
foreach(want "\"name\": \"BLK1.LED-ANODE\"" "\"name\": \"BLK2.LED-ANODE\"")
    string(FIND "${blinky_nets}" "${want}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "expected a net named ${want}")
    endif()
endforeach()

# The indicator imports the board's ground with '>>GND' (spec 10.3), so the
# design has exactly ONE net named GND and the LED cathodes sit on it with the
# rest of the board -- not on a private one-pin ground per instance.
string(JSON blinky_net_count LENGTH "${blinky_nets}" nets)
math(EXPR blinky_net_last "${blinky_net_count} - 1")
set(gnd_count 0)
foreach(i RANGE ${blinky_net_last})
    string(JSON gnd_name GET "${blinky_nets}" nets ${i} name)
    if(gnd_name STREQUAL "GND")
        math(EXPR gnd_count "${gnd_count} + 1")
    endif()
endforeach()
if(NOT gnd_count EQUAL 1)
    message(FATAL_ERROR "expected exactly one net named GND, got ${gnd_count}")
endif()
assert_same_net(blinky_nets J1 2 BLK1_D1 1)
assert_same_net(blinky_nets J1 2 BLK2_D1 1)

# The mirrored debug lead: '@map = [[1:5, 5:1]]' on the plug, so header pin 1
# (3V3) is met by the loom's conductor 5. The board's own netlist is unchanged
# by the map; the check is that the mating passed under -Werror above.

# --- render: the two 'indicator' copies share one page -----------------------
# The schematic is blinky's top page plus ONE page for the 'indicator'
# definition: BLK1 and BLK2 draw as green sheet symbols on the top page, both
# linking to that single page, which holds the R-LED chain once under its
# block-local net spellings.
run_manta(render -Werror -o "${WORK}/blinky.html" "${WORK}/blinky.mantaNets")
file(READ "${WORK}/blinky.html" blinky_html)

foreach(page "id=\"page-blinky\"" "id=\"page-indicator\"")
    string(REGEX MATCHALL "${page}" pages "${blinky_html}")
    list(LENGTH pages page_count)
    if(NOT page_count EQUAL 1)
        message(FATAL_ERROR "expected exactly one ${page}, got ${page_count}")
    endif()
endforeach()

# Two sheet symbols; three links to the indicator page (the third is the
# sidebar's own entry).
string(REGEX MATCHALL "class=\"sbody\"" sheets "${blinky_html}")
list(LENGTH sheets sheet_count)
if(NOT sheet_count EQUAL 2)
    message(FATAL_ERROR "expected sheet symbols for BLK1 and BLK2, got ${sheet_count}")
endif()
string(REGEX MATCHALL "href=\"#page-indicator\"" links "${blinky_html}")
list(LENGTH links link_count)
if(NOT link_count EQUAL 3)
    message(FATAL_ERROR "expected 3 links to page-indicator, got ${link_count}")
endif()

# The representative's parts render once; the second instance's not at all.
foreach(des BLK1_R1 BLK1_D1)
    string(REGEX MATCHALL "data-c=\"${des}\"" syms "${blinky_html}")
    list(LENGTH syms sym_count)
    if(NOT sym_count EQUAL 1)
        message(FATAL_ERROR "expected ${des} drawn exactly once, got ${sym_count}")
    endif()
endforeach()
if(blinky_html MATCHES "data-c=\"BLK2_R1\"")
    message(FATAL_ERROR "BLK2's copy rendered: the 2nd instance must draw no page")
endif()

# Local spellings on the indicator page: the DRIVE port flag, with data-net
# still the flat design-wide name so a click highlights it on the top page too.
if(NOT blinky_html MATCHES "class=\"portflag\" data-net=\"LED-DRIVE\\[0\\]\"")
    message(FATAL_ERROR "the indicator page has no DRIVE port flag on LED-DRIVE[0]")
endif()
if(NOT blinky_html MATCHES ">DRIVE<")
    message(FATAL_ERROR "the local port spelling DRIVE is drawn nowhere")
endif()
if(NOT blinky_html MATCHES "data-net=\"BLK1.LED-ANODE\"")
    message(FATAL_ERROR "the indicator's chain wire does not carry the flat net name")
endif()
if(NOT blinky_html MATCHES ">instances: BLK1, BLK2<")
    message(FATAL_ERROR "the indicator page does not list its instances")
endif()

# The '--- TITLE' markers in board.manta become titled rooms on the top page.
foreach(room "USB-C POWER IN" "3V3 REGULATOR" "VBUS SENSE" "MCU" "I2C" "CONFIG"
             "INDICATORS" "DEBUG")
    if(NOT blinky_html MATCHES "class=\"roomtitle\"[^>]*>${room}<")
        message(FATAL_ERROR "no room titled '${room}' on the rendered page")
    endif()
endforeach()

# Rules must not perturb the netlist, and must be deterministic.
run_manta(link --top blinky -L "${WORK}/build" --rules "${RULES}" -Werror
          -o "${WORK}/blinky2.mantaNets")
file(SHA256 "${WORK}/blinky.mantaNets" a)
file(SHA256 "${WORK}/blinky2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "linking with rules is not deterministic")
endif()

# The lead is its own deliverable: it links on its own, and '--assembly' writes
# it beside the board without ever merging the two.
run_manta(link --top usb-c-1m -L "${WORK}/build" -Werror
          -o "${WORK}/lead.mantaNets" --bom "${WORK}/lead.csv")
file(STRINGS "${WORK}/lead.csv" wires REGEX ",wire,")
file(STRINGS "${WORK}/lead.csv" crimps REGEX ",crimp,")
if(NOT wires OR NOT crimps)
    message(FATAL_ERROR "the lead's BOM lists no wires or no crimps")
endif()

file(REMOVE "${WORK}/usb-c-1m.mantaNets")
execute_process(COMMAND "${MANTA}" link --top blinky -L "${WORK}/build" --rules "${RULES}"
                        -Werror --assembly -o "${WORK}/blinky3.mantaNets"
                        --bom "${WORK}/blinky3.csv"
                WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE asm_code)
if(NOT asm_code EQUAL 0)
    message(FATAL_ERROR "--assembly failed on blinky")
endif()
foreach(lead usb-c-1m swd-lead i2c-lead)
    if(NOT EXISTS "${WORK}/${lead}.mantaNets")
        message(FATAL_ERROR "--assembly wrote no netlist for the mated lead ${lead}")
    endif()
endforeach()
file(SHA256 "${WORK}/blinky.mantaNets" f)
file(SHA256 "${WORK}/blinky3.mantaNets" g)
if(NOT f STREQUAL g)
    message(FATAL_ERROR "--assembly changed the board's own netlist")
endif()

message(STATUS "example: blinky passes check, link, rules, mating and export with no findings")
