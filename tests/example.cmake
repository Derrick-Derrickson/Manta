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

# Rules must not perturb the netlist, and must be deterministic.
run_manta(link --top blinky -L "${WORK}/build" --rules "${RULES}" -Werror
          -o "${WORK}/blinky2.mantaNets")
file(SHA256 "${WORK}/blinky.mantaNets" a)
file(SHA256 "${WORK}/blinky2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "linking with rules is not deterministic")
endif()

message(STATUS "example: blinky passes check, link, rules and export with no findings")
