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

# A decorated design compiles with no rules file: the '#' fields it carries are
# ordinary manta, so the rules are pure checking and never a build dependency.
# Compiling *with* them additionally runs the part-domain checks.
run_manta(compile -o "${WORK}/build/" --rules "${RULES}" ${EXAMPLE_SOURCES})

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

# Rules must not perturb the netlist, and must be deterministic.
run_manta(link --top blinky -L "${WORK}/build" --rules "${RULES}" -Werror
          -o "${WORK}/blinky2.mantaNets")
file(SHA256 "${WORK}/blinky.mantaNets" a)
file(SHA256 "${WORK}/blinky2.mantaNets" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "linking with rules is not deterministic")
endif()

message(STATUS "example: blinky passes check, link, rules and export with no findings")
