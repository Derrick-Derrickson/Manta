# Revision 1.4, spec 7.4: "A binding is a chain rooted at a pin of the enclosing
# instance." Everything here exists because a chain binding can parse, format and
# round-trip through .mantaO perfectly and still connect nothing -- a wrong answer
# with no diagnostic attached to it.
#
# Expects: MANTA (path to the binary), DIR (tests/bindings), SPEC_DIR (tests/spec),
# WORK (scratch dir).

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
# The equivalence property
# ---------------------------------------------------------------------------
# bound.manta and hoisted.manta are one design written two ways: every chain
# that sits in a binding list in the first is a statement of the enclosing body
# in the second, in the same order, with nothing else changed. Spec 7.4 says the
# two spellings mean the same thing, so their netlists must agree byte for byte.
#
# Between them they cover a shunt capacitor off a pin, a two-resistor divider, a
# series element between a pin and a net, a series element between two pins, a
# '==' pair bracketing a device onto the pin's node, a device nested two levels deep
# inside a binding, and array-pin bindings against both a replication and a net
# that states no range of its own.
run_manta(compile -o "${WORK}/bound/" "${DIR}/parts.manta" "${DIR}/bound.manta")
run_manta(compile -o "${WORK}/hoisted/" "${DIR}/parts.manta" "${DIR}/hoisted.manta")

run_manta(link --top eq-top -L "${WORK}/bound" --no-erc -o "${WORK}/bound.mantaNets")
run_manta(link --top eq-top -L "${WORK}/hoisted" --no-erc -o "${WORK}/hoisted.mantaNets")

file(SHA256 "${WORK}/bound.mantaNets" bound_hash)
file(SHA256 "${WORK}/hoisted.mantaNets" hoisted_hash)
if(NOT bound_hash STREQUAL hoisted_hash)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
                            "${WORK}/bound.mantaNets" "${WORK}/hoisted.mantaNets")
    message(FATAL_ERROR "a binding chain and its hoisted form linked to different netlists")
endif()

# Spec 15.8, on the path that now creates components mid-instantiation.
run_manta(link --top eq-top -L "${WORK}/bound" --no-erc -o "${WORK}/bound2.mantaNets")
file(SHA256 "${WORK}/bound2.mantaNets" bound_again)
if(NOT bound_hash STREQUAL bound_again)
    message(FATAL_ERROR "linking a design with chain bindings is not deterministic")
endif()

# An identical netlist would also be identically empty, so the connections are
# named outright. A '==' pair has to open at the pin: U1.SW carries the pin, the
# diode's cathode, the bootstrap capacitor and the far end of R5.
file(READ "${WORK}/bound.mantaNets" bound_net)
foreach(want "\"RS4\"" "\"C3\"" "\"SIDE-BUS[3]\"" "\"LANE-OUT[3]\"")
    string(FIND "${bound_net}" "${want}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "a binding chain created nothing for ${want}")
    endif()
endforeach()

function(assert_net_pins netsvar name)
    string(JSON net_count LENGTH "${${netsvar}}" nets)
    math(EXPR net_last "${net_count} - 1")
    foreach(i RANGE ${net_last})
        string(JSON n GET "${${netsvar}}" nets ${i} name)
        if(NOT n STREQUAL name)
            continue()
        endif()
        string(JSON pins GET "${${netsvar}}" nets ${i} pins)
        string(JSON pin_count LENGTH "${pins}")
        set(got "")
        math(EXPR pin_last "${pin_count} - 1")
        foreach(p RANGE ${pin_last})
            string(JSON d GET "${pins}" ${p} designator)
            string(JSON pn GET "${pins}" ${p} pin)
            list(APPEND got "${d}.${pn}")
        endforeach()
        list(SORT got)
        set(want ${ARGN})
        list(SORT want)
        if(NOT got STREQUAL want)
            message(FATAL_ERROR "net '${name}' carries ${got}, expected ${want}")
        endif()
        return()
    endforeach()
    message(FATAL_ERROR "no net named '${name}'")
endfunction()

# The '==' pair opens at the pin (spec 6.3): "SW == K{D1} == .{C2}" is one net.
assert_net_pins(bound_net "U1.SW" U1.2 D1.2 C2.2 R5.2)
# ...and a bracket that reaches a device by only one terminal does not short it.
assert_net_pins(bound_net "GND" U1.6 C1.1 D1.1 R2.1 C3.1)
# The divider's midpoint is the pin itself.
assert_net_pins(bound_net "U1.FB" U1.3 R1.2 R2.2)
# Two levels deep: R4 hangs off COMP, C3 hangs off R4.
assert_net_pins(bound_net "R4.A" R4.1 C3.2)

# ---------------------------------------------------------------------------
# W-01 counts a chain-bound pin as bound
# ---------------------------------------------------------------------------
# Spec 16.2: "A declared part has pins appearing in no chain and no binding."
# A pin bound to a chain appears in a binding. COMP below is the only pin of U1
# that does not, so W-01 must report one and not six.
file(WRITE "${WORK}/w01.manta" "\
block w01-top {
    GND &TYPE=GROUND;
    {U1~EQ-CHIP:
        .VIN  = VRAIL = .{C1~EQ-C: . = GND;};
        .SW   = K.{D1~EQ-D: .A = GND;};
        .FB   = .{R1~EQ-R: . = GND;};
        .EN   = VRAIL;
        .GND  = GND;
    };
};
")
run_manta(compile -o "${WORK}/w01/" "${DIR}/parts.manta" "${WORK}/w01.manta")
execute_process(COMMAND "${MANTA}" link --top w01-top -L "${WORK}/w01"
                        -Wno-E-01 -Wno-E-02 -Wno-E-26 -Wno-W-03
                        -o "${WORK}/w01.mantaNets"
                ERROR_VARIABLE w01_err RESULT_VARIABLE w01_code)
if(NOT w01_code EQUAL 0)
    message(FATAL_ERROR "the W-01 fixture did not link:\n${w01_err}")
endif()
if(NOT w01_err MATCHES "W-01[^\n]*'U1' has 1 pins")
    message(FATAL_ERROR "expected W-01 to count exactly one unbound pin, got:\n${w01_err}")
endif()

# ...and the warning still fires for pins that really are unbound: with the
# bindings removed, all six are.
file(WRITE "${WORK}/w01-bare.manta" "block w01-bare { {U1~EQ-CHIP}; };\n")
run_manta(compile -o "${WORK}/w01bare/" "${DIR}/parts.manta" "${WORK}/w01-bare.manta")
execute_process(COMMAND "${MANTA}" link --top w01-bare -L "${WORK}/w01bare"
                        -o "${WORK}/w01-bare.mantaNets"
                ERROR_VARIABLE bare_err RESULT_VARIABLE bare_code)
if(NOT bare_err MATCHES "W-01[^\n]*'U1' has 6 pins")
    message(FATAL_ERROR "W-01 stopped firing for genuinely unbound pins:\n${bare_err}")
endif()

# ---------------------------------------------------------------------------
# E-08 reaches a chain binding
# ---------------------------------------------------------------------------
# Spec 7.4: "A pin used as a terminal shall not also appear in the binding list
# (E-08), whether that binding carries a single net or a chain."
file(WRITE "${WORK}/e08.manta" "\
block e08-top {
    GND &TYPE=GROUND;
    VRAIL = A.{R9~EQ-R: .A = .{C9~EQ-C: . = GND;};}.B = GND;
};
")
execute_process(COMMAND "${MANTA}" compile -o "${WORK}/e08/"
                        "${DIR}/parts.manta" "${WORK}/e08.manta"
                ERROR_VARIABLE e08_err RESULT_VARIABLE e08_code)
if(e08_code EQUAL 0)
    message(FATAL_ERROR "a pin used as a terminal and bound to a chain compiled cleanly")
endif()
if(NOT e08_err MATCHES "E-08")
    message(FATAL_ERROR "expected E-08 for a chain binding, got: ${e08_err}")
endif()

# The same shape with a single net after '=' has always been E-08, and still is.
file(WRITE "${WORK}/e08-net.manta" "\
block e08-net-top {
    GND &TYPE=GROUND;
    VRAIL = A.{R9~EQ-R: .A = GND;}.B = GND;
};
")
execute_process(COMMAND "${MANTA}" compile -o "${WORK}/e08net/"
                        "${DIR}/parts.manta" "${WORK}/e08-net.manta"
                ERROR_VARIABLE e08net_err RESULT_VARIABLE e08net_code)
if(e08net_code EQUAL 0 OR NOT e08net_err MATCHES "E-08")
    message(FATAL_ERROR "E-08 stopped firing for 'PIN = NET': ${e08net_err}")
endif()

# ---------------------------------------------------------------------------
# 'manta annotate' reaches inside a binding list
# ---------------------------------------------------------------------------
# Spec 7.4: "A device declared inside a binding is an ordinary instance of the
# enclosing body ... and is annotated with everything else (13)." Annotation
# rewrites source, and this is the first path on which a designator lives inside
# a binding list rather than in a statement of the body.
set(ANNSRC "${WORK}/ann.manta")
file(WRITE "${ANNSRC}" "\
block ann-top {
    GND &TYPE=GROUND;
    {U1~EQ-CHIP:
        .VIN  = VRAIL = .{C?~EQ-C: . = .{R?~EQ-R: . = GND;};};
        .SW   = K.{D?~EQ-D: .A = GND;};
        .GND  = GND;
    };
};
")
run_manta(compile -o "${WORK}/ann/" "${DIR}/parts.manta" "${ANNSRC}")
run_manta(link --top ann-top -L "${WORK}/ann" --no-erc -Wno-unannotated
          -o "${WORK}/ann.mantaNets")
run_manta(annotate -n "${WORK}/ann.mantaNets" "${ANNSRC}")

file(READ "${ANNSRC}" ann_after)
if(ann_after MATCHES "[?]")
    message(FATAL_ERROR "annotate left a '?' inside a binding list:\n${ann_after}")
endif()
# Numbers follow source order, and the nested device is numbered after the one
# whose binding list holds it (spec 13.3).
if(NOT ann_after MATCHES "C1~EQ-C" OR NOT ann_after MATCHES "R1~EQ-R"
   OR NOT ann_after MATCHES "D1~EQ-D")
    message(FATAL_ERROR "annotate numbered a binding's devices wrongly:\n${ann_after}")
endif()

# The annotated source still links, now with nothing unassigned.
run_manta(compile -o "${WORK}/ann2/" "${DIR}/parts.manta" "${ANNSRC}")
run_manta(link --top ann-top -L "${WORK}/ann2" --no-erc -o "${WORK}/ann2.mantaNets")
file(STRINGS "${WORK}/ann2.mantaNets" leftover REGEX "\"designator\": \"[^\"]*[?]")
if(leftover)
    message(FATAL_ERROR "an unassigned designator inside a binding survived: ${leftover}")
endif()

# Spec 13.6: annotating again changes nothing.
file(READ "${ANNSRC}" ann_before_second)
run_manta(annotate -n "${WORK}/ann2.mantaNets" "${ANNSRC}")
file(READ "${ANNSRC}" ann_after_second)
if(NOT ann_before_second STREQUAL ann_after_second)
    message(FATAL_ERROR "annotate renumbered a designator inside a binding list")
endif()

# ---------------------------------------------------------------------------
# tests/spec/bindings.manta is elaborated, not merely parsed
# ---------------------------------------------------------------------------
# The spec fixture is reachable from the pipeline's top block, but it is linked
# on its own here too so that its connections can be named. A design the
# pipeline only ever parsed is exactly how a silent elaboration bug escapes.
file(GLOB SPEC_SOURCES "${SPEC_DIR}/*.manta")
run_manta(compile -o "${WORK}/spec/" ${SPEC_SOURCES})
run_manta(link --top bindings-demo -L "${WORK}/spec" --no-erc -Wno-unannotated
          -o "${WORK}/demo.mantaNets")
file(READ "${WORK}/demo.mantaNets" demo_net)

# The buck's own bindings, as spec 7.4 writes them.
assert_net_pins(demo_net "U2.SW" U2.2 D2.2 C3.2)
assert_net_pins(demo_net "U2.FB" U2.3 R3.2 R4.2)
assert_net_pins(demo_net "U2.EN" U2.4 R5.1)
assert_net_pins(demo_net "VPOS-PROT" U2.1 C1.2 R5.2 R7.1 C5.2 R8.2)
# '=*' opens a binding as it opens a statement: four pins onto one net.
assert_net_pins(demo_net "LANE-COMMON" U3.1 U3.2 U3.3 U3.4)
# Two levels deep inside a binding list.
assert_net_pins(demo_net "R6.A" R6.1 C4.2)

message(STATUS "bindings: chain bindings elaborate exactly as their hoisted form")
