# Multi-pin room-local nets: a net joining three or more pins is DRAWN as one
# conductor tree, instead of the same label repeated once per pin.
#
# History: this fixture was written for the classic node idiom (a trunk with
# taps rooted at an anchor), whose guards included nets the idiom declined
# (UNCLAIMED), trunks too narrow for their tap names (WIDE) and taps the
# drawing could not hold (PAIRED's third pin). The flow placer routes
# room-local nets outright, so every one of those nets is now a drawn
# conductor -- strictly better than the labels the idiom fell back to. The
# electrical-correctness intent is unchanged and still enforced here:
#
#   - a drawn net carries no label at all, anywhere;
#   - a junction dot appears where three or more conductors meet and NOWHERE
#     else -- not at a corner, not where two conductors join end to end;
#   - a net the drawing leaves out (the single-pin far ends) keeps its label,
#     because a pin showing neither a wire nor a mark is the one outcome no
#     degradation may produce (the router's refusal path is proven at unit
#     level in test_route.cpp, and the layout asserts every placed pin keeps
#     a conductor);
#   - the whole sheet stays byte-reproducible (spec 15.8).
#
# Expects: MANTA (path to the binary), WORK (scratch dir).

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

# Counts occurrences of a regex in the rendered HTML.
function(count_matches out_var text pattern)
    string(REGEX MATCHALL "${pattern}" hits "${text}")
    list(LENGTH hits n)
    set(${out_var} ${n} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# U1 is the anchor every net roots at. The nets under test:
#
#   NODE3    U1.5, R1.2, C1.2                     three pins: pull-up + pull-down
#   NODE4    U1.6, R2.2, C2.2, L1.2               four pins
#   PAIRED   U1.2, U1.3, J1.1                     two pins of ONE anchor plus a
#                                                 third body
#   WIDE     U1.7, R3.2, C3.2                     taps whose far-end names are
#                                                 very wide single-pin nets
#   UNCLAIMED U1.4, SW1.1, J2.1                   one pin on each of three
#                                                 bodies
#   STRUNG   U1.9, R5.1, C4.2                     a net that continues through
#                                                 a second part before ground
set(SRC "${WORK}/nodes.manta")
file(WRITE "${SRC}" "\
netclass power { &CURRENT=1A; };

part FIX-R { @!type = resistor; @~footprint = R-0603; #value = 10kR; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-C { @!type = capacitor; @~footprint = C-0603; #value = 100nF; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-L { @!type = inductor; @~footprint = L-0805; #value = 4u7H; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-LED { @!type = led; @~footprint = R-0603; #value = red; 1 : A; 2 : K; };
part FIX-SW {
    @!type = switch;
    @~footprint = SW-SMD;
    1 : A1 &CASUAL; 2 : A2 &CASUAL; 3 : B1 &CASUAL; 4 : B2 &CASUAL;
};
part FIX-J { @!type = boardconnector; @~footprint = HDR-1x2; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-U {
    @~footprint = QFP-32;
    1 : VCC< &TYPE=POWER;
    2 : IOA;
    3 : IOB;
    4 : BTN<;
    5 : FB<;
    6 : SW>;
    7 : WIDEPIN;
    8 : GND< &TYPE=POWER &~NET=GND;
    9 : STR>;
};

block fixture {
    GND &TYPE=GROUND;
    3V3 &CLASS=power;

    {U1~FIX-U:
        .VCC = 3V3; .GND = GND;
        .IOA = PAIRED; .IOB = PAIRED;
        .BTN = UNCLAIMED;
        .FB = NODE3;
        .SW = NODE4;
        .WIDEPIN = WIDE;
        .STR = STRUNG;
    };

    // STRUNG carries on through a second part before it lands, the way an
    // LED and its series resistor do. STR-MID is inside that string, so it
    // must be a drawn conductor and never a label.
    {R5~FIX-R: .A = STRUNG; .B = STR-MID;};
    {D1~FIX-LED: .A = STR-MID; .K = GND;};
    {C4~FIX-C: .A = GND; .B = STRUNG;};

    // NODE3: a divider tap. R1 goes up to the rail, C1 down to ground.
    {R1~FIX-R: .A = 3V3; .B = NODE3;};
    {C1~FIX-C: .A = GND; .B = NODE3;};

    // NODE4: the switch-node shape -- catch part down, cap down, coil up.
    {R2~FIX-R: .A = GND; .B = NODE4;};
    {C2~FIX-C: .A = BUCK-BOOT; .B = NODE4;};
    {L1~FIX-L: .A = 3V3; .B = NODE4;};

    // PAIRED joins two pins of U1 and a third body's pin.
    {J1~FIX-J: .A = PAIRED; .B = GND;};

    // BUCK-BOOT joins exactly two pins: a pair, and never a junction.
    {R4~FIX-R: .A = BUCK-BOOT; .B = GND;};

    // WIDE: the far-end names are absurdly wide; drawing must not care.
    {R3~FIX-R: .A = A-FAR-END-NET-NAME-FAR-TOO-LONG-TO-SIT-UNDER-ANY-TRUNK-AT-ALL-1; .B = WIDE;};
    {C3~FIX-C: .A = A-FAR-END-NET-NAME-FAR-TOO-LONG-TO-SIT-UNDER-ANY-TRUNK-AT-ALL-2; .B = WIDE;};

    // UNCLAIMED: one pin on each of three bodies, no two-terminal part
    // between them -- the shape the classic trunk idiom had to decline.
    {SW1~FIX-SW: .A1 = UNCLAIMED; .A2 = GND; .B1 = GND; .B2 = GND;};
    {J2~FIX-J: .A = UNCLAIMED; .B = GND;};
};
")

run_manta(compile -o "${WORK}/build/" "${SRC}")
run_manta(link --top fixture -L "${WORK}/build" --no-erc -o "${WORK}/nodes.mantaNets")

# --- determinism (spec 15.8): two renders, identical bytes -------------------
run_manta(render -o "${WORK}/nodes.html" "${WORK}/nodes.mantaNets")
run_manta(render -o "${WORK}/nodes2.html" "${WORK}/nodes.mantaNets")
file(SHA256 "${WORK}/nodes.html" a)
file(SHA256 "${WORK}/nodes2.html" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "a sheet carrying multi-way nets is not byte-reproducible")
endif()

file(READ "${WORK}/nodes.html" html)

# --- every multi-pin room-local net is one drawn conductor, labelled nowhere --
# Under the classic idiom UNCLAIMED and WIDE degraded to labels and PAIRED
# kept a mark for the pin its trunk could not hold; the router joins all of
# them, so the guard is now uniform: drawn, and no label anywhere.
foreach(net "NODE3" "NODE4" "PAIRED" "UNCLAIMED" "WIDE" "STRUNG")
    if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"${net}\"")
        message(FATAL_ERROR "${net} is drawn as a routed conductor and must carry no label")
    endif()
    if(NOT html MATCHES "class=\"wire\"[^>]*data-net=\"${net}\"")
        message(FATAL_ERROR "${net} must be drawn as a wire")
    endif()
endforeach()

# A three-pin net needs at least its three stubs and the joining tree.
count_matches(n3_wires "${html}" "class=\"wire\"[^>]*data-net=\"NODE3\"")
if(n3_wires LESS 3)
    message(FATAL_ERROR "NODE3 joins three pins and should carry at least 3 wire "
                        "elements, got ${n3_wires}")
endif()
count_matches(n4_wires "${html}" "class=\"wire\"[^>]*data-net=\"NODE4\"")
if(n4_wires LESS 4)
    message(FATAL_ERROR "NODE4 joins four pins and should carry at least 4 wire "
                        "elements, got ${n4_wires}")
endif()

# --- junction dots: three or more conductors, and only there -----------------
# A routed tree of n pins branches n-2 times when every branch is a T, and a
# branch landing on an endpoint is a corner instead. On this fixture the
# routes are deterministic: NODE3's tree has one T, NODE4's two.
count_matches(n3_dots "${html}" "class=\"dot\"[^>]*data-net=\"NODE3\"")
if(NOT n3_dots EQUAL 1)
    message(FATAL_ERROR "NODE3's tree has one three-way junction, so exactly one "
                        "dot; got ${n3_dots}")
endif()
count_matches(n4_dots "${html}" "class=\"dot\"[^>]*data-net=\"NODE4\"")
if(NOT n4_dots EQUAL 2)
    message(FATAL_ERROR "NODE4's tree has two three-way junctions, so exactly two "
                        "dots; got ${n4_dots}")
endif()

# A dot is never drawn on a net whose conductors only ever meet pairwise.
# BUCK-BOOT joins exactly two pins: whatever shape its wire takes, no point
# of it may collect three conductor ends.
count_matches(bb_dots "${html}" "class=\"dot\"[^>]*data-net=\"BUCK-BOOT\"")
if(NOT bb_dots EQUAL 0)
    message(FATAL_ERROR "a two-pin net must never grow a junction dot")
endif()

# --- the parts of every drawn net are still on the sheet ---------------------
foreach(part "R3" "C3" "R5" "D1" "SW1" "J1" "J2")
    if(NOT html MATCHES ">${part}<")
        message(FATAL_ERROR "${part} belongs to a drawn net but is not on the sheet")
    endif()
endforeach()

# --- a net the drawing leaves out keeps its label ------------------------------
# The far ends of R3 and C3 are single-pin nets: nothing to route, so each
# keeps its (very wide) label -- losing it would disconnect the pin, the one
# unacceptable outcome. This is the fallback contract's e2e face; the
# router's refusal path itself is unit-tested in test_route.cpp.
foreach(far "1" "2")
    if(NOT html MATCHES "class=\"netlabel\"[^>]*data-net=\"A-FAR-END-NET-NAME-FAR-TOO-LONG-TO-SIT-UNDER-ANY-TRUNK-AT-ALL-${far}\"")
        message(FATAL_ERROR "a single-pin net lost its label and with it the pin's connection")
    endif()
endforeach()

# --- a string's interior net stays fully drawn --------------------------------
# STRUNG continues through R5 into D1: STR-MID is inside that string and must
# be a drawn conductor carrying no mark of any kind -- a drawing that
# un-draws an interior net would leave two half-conductors.
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"STR-MID\"")
    message(FATAL_ERROR "STR-MID is inside a drawn string and must not be labelled")
endif()
if(NOT html MATCHES "class=\"wire\"[^>]*data-net=\"STR-MID\"")
    message(FATAL_ERROR "STR-MID is inside a drawn string and must be drawn")
endif()

message(STATUS "render nodes: routed multi-pin nets, junction dots, single-pin "
               "labels and determinism all verified")
