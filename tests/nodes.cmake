# Multi-way nodes: a net joining three or more pins drawn as a trunk with
# taps, instead of the same label repeated once per pin.
#
# The properties under test are the ones a reader depends on: a node the idiom
# claims is DRAWN and carries no label at all; a junction dot appears where
# three or more conductors meet and NOWHERE else -- not at a corner, not where
# two conductors join end to end; a node the idiom cannot place degrades to
# labels on every pin, with its parts still on the sheet; and the whole sheet
# stays byte-reproducible (spec 15.8).
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

# U1 is the anchor every node roots at. The nets under test:
#
#   NODE3    U1.5, R1.2, C1.2                     three pins: anchor + 2 taps
#   NODE4    U1.6, R2.2, C2.2, L1.2               four pins:  anchor + 3 taps
#   PAIRED   U1.2, U1.3, J1.1                     two pins of ONE anchor, and a
#                                                 third the drawing cannot hold
#   WIDE     U1.7, R3.2, C3.2                     taps whose far-end names are
#                                                 too wide for any trunk
#   UNCLAIMED U1.4, SW1.1, J2.1                   one pin on each of three
#                                                 bodies: the idiom declines it
#
# A tap to a rail stands ABOVE the trunk and one to ground hangs below, so
# NODE3 is a divider read the way a divider is drawn.
set(SRC "${WORK}/nodes.manta")
file(WRITE "${SRC}" "\
netclass power { &CURRENT=1A; };

part FIX-R { @!type = resistor; @~footprint = R-0603; #value = 10kR; 1 = A &CASUAL; 2 = B &CASUAL; };
part FIX-C { @!type = capacitor; @~footprint = C-0603; #value = 100nF; 1 = A &CASUAL; 2 = B &CASUAL; };
part FIX-L { @!type = inductor; @~footprint = L-0805; #value = 4u7H; 1 = A &CASUAL; 2 = B &CASUAL; };
part FIX-SW {
    @!type = switch;
    @~footprint = SW-SMD;
    1 = A1 &CASUAL; 2 = A2 &CASUAL; 3 = B1 &CASUAL; 4 = B2 &CASUAL;
};
part FIX-J { @!type = boardconnector; @~footprint = HDR-1x2; 1 = A &CASUAL; 2 = B &CASUAL; };
part FIX-U {
    @~footprint = QFP-32;
    1 = VCC< &TYPE=POWER;
    2 = IOA;
    3 = IOB;
    4 = BTN<;
    5 = FB<;
    6 = SW>;
    7 = WIDEPIN;
    8 = GND< &TYPE=POWER &~NET=GND;
};

block fixture {
    GND &TYPE=GROUND;
    3V3 &CLASS=power;

    {U1~FIX-U:
        VCC = 3V3; GND = GND;
        IOA = PAIRED; IOB = PAIRED;
        BTN = UNCLAIMED;
        FB = NODE3;
        SW = NODE4;
        WIDEPIN = WIDE;
    };

    // NODE3: a divider tap. R1 goes up to the rail, C1 down to ground.
    {R1~FIX-R: A = 3V3; B = NODE3;};
    {C1~FIX-C: A = GND; B = NODE3;};

    // NODE4: the switch-node shape -- catch part down, cap down, coil up.
    {R2~FIX-R: A = GND; B = NODE4;};
    {C2~FIX-C: A = BUCK-BOOT; B = NODE4;};
    {L1~FIX-L: A = 3V3; B = NODE4;};

    // PAIRED reaches a pin the trunk cannot hold, so the trunk keeps a mark.
    {J1~FIX-J: A = PAIRED; B = GND;};

    // BUCK-BOOT joins exactly two pins: the chain idiom's business, not a
    // node's, and never a junction.
    {R4~FIX-R: A = BUCK-BOOT; B = GND;};

    // WIDE: the far-end names are wider than any trunk the sheet will carry.
    {R3~FIX-R: A = A-FAR-END-NET-NAME-FAR-TOO-LONG-TO-SIT-UNDER-ANY-TRUNK-AT-ALL-1; B = WIDE;};
    {C3~FIX-C: A = A-FAR-END-NET-NAME-FAR-TOO-LONG-TO-SIT-UNDER-ANY-TRUNK-AT-ALL-2; B = WIDE;};

    // UNCLAIMED: one pin on each of three bodies and no two-terminal part
    // anywhere on it, so no anchor can root a trunk that reaches the others.
    {SW1~FIX-SW: A1 = UNCLAIMED; A2 = GND; B1 = GND; B2 = GND;};
    {J2~FIX-J: A = UNCLAIMED; B = GND;};
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
    message(FATAL_ERROR "a sheet carrying multi-way nodes is not byte-reproducible")
endif()

file(READ "${WORK}/nodes.html" html)

# --- a three-pin node is drawn, and labelled nowhere -------------------------
# Every pin of NODE3 is on the trunk, so the trunk needs no mark of its own:
# the net must not appear as a label anywhere on the sheet.
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"NODE3\"")
    message(FATAL_ERROR "NODE3 joins three pins and is drawn: it must carry no label")
endif()
count_matches(n3_wires "${html}" "class=\"wire\"[^>]*data-net=\"NODE3\"")
if(n3_wires LESS 3)
    message(FATAL_ERROR "NODE3 should be drawn as a trunk with two taps, "
                        "got ${n3_wires} wires")
endif()

# --- a four-pin node is drawn, and labelled nowhere --------------------------
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"NODE4\"")
    message(FATAL_ERROR "NODE4 joins four pins and is drawn: it must carry no label")
endif()
count_matches(n4_wires "${html}" "class=\"wire\"[^>]*data-net=\"NODE4\"")
if(n4_wires LESS 4)
    message(FATAL_ERROR "NODE4 should be drawn as a trunk with three taps, "
                        "got ${n4_wires} wires")
endif()

# --- junction dots: three or more conductors, and only there -----------------
# NODE3's trunk carries two taps and stops dead on the second. The first tap is
# a T -- trunk in, trunk on, tap down -- and takes a dot. The second is a
# corner, two conductors meeting end to end, and takes none. So: exactly one.
count_matches(n3_dots "${html}" "class=\"dot\"[^>]*data-net=\"NODE3\"")
if(NOT n3_dots EQUAL 1)
    message(FATAL_ERROR "NODE3 has one T and one corner, so exactly one junction "
                        "dot; got ${n3_dots}")
endif()

# NODE4 carries three taps and stops on the third: two Ts, one corner.
count_matches(n4_dots "${html}" "class=\"dot\"[^>]*data-net=\"NODE4\"")
if(NOT n4_dots EQUAL 2)
    message(FATAL_ERROR "NODE4 has two Ts and one corner, so exactly two junction "
                        "dots; got ${n4_dots}")
endif()

# PAIRED joins two pins of U1 with a spine and runs on to a mark, because J1.1
# is left outside the drawing. The far pin's leg meets the spine end-on -- a
# corner, no dot -- and the near pin's leg meets both the spine and the trunk,
# which is three conductors and one dot.
count_matches(pr_dots "${html}" "class=\"dot\"[^>]*data-net=\"PAIRED\"")
if(NOT pr_dots EQUAL 1)
    message(FATAL_ERROR "PAIRED's spine has one three-way junction and one corner, "
                        "so exactly one dot; got ${pr_dots}")
endif()
if(NOT html MATCHES "data-net=\"PAIRED\"")
    message(FATAL_ERROR "PAIRED was not drawn at all")
endif()
# The trunk must still carry its mark: J1.1 connects to it by name.
count_matches(pr_marks "${html}" "class=\"netlabel\"[^>]*data-net=\"PAIRED\"")
if(pr_marks LESS 2)
    message(FATAL_ERROR "PAIRED reaches J1.1, which is off the trunk, so both the "
                        "trunk and that pin must be labelled; got ${pr_marks}")
endif()

# A dot is never drawn on a net that is only stubs and labels.
foreach(plain "UNCLAIMED" "WIDE" "BUCK-BOOT")
    count_matches(d "${html}" "class=\"dot\"[^>]*data-net=\"${plain}\"")
    if(NOT d EQUAL 0)
        message(FATAL_ERROR "${plain} is not drawn as a node and must carry no "
                            "junction dot; got ${d}")
    endif()
endforeach()

# --- the trunk that cannot be placed falls back to labels --------------------
# WIDE's taps carry far-end names wider than any trunk the sheet will hold, so
# the node gives every part back. All three pins must keep a label, and the
# parts themselves must still be on the sheet with their own labels -- a pin
# showing neither a wire nor a mark is the one unacceptable outcome.
count_matches(wide_labels "${html}" "class=\"netlabel\"[^>]*data-net=\"WIDE\"")
if(wide_labels LESS 3)
    message(FATAL_ERROR "WIDE's trunk cannot be placed, so each of its three pins "
                        "keeps a label; got ${wide_labels}")
endif()
foreach(part "R3" "C3")
    if(NOT html MATCHES ">${part}<")
        message(FATAL_ERROR "${part} was claimed by a node that could not be drawn "
                            "and then left off the sheet entirely")
    endif()
endforeach()
foreach(far "1" "2")
    if(NOT html MATCHES "data-net=\"A-FAR-END-NET-NAME-FAR-TOO-LONG-TO-SIT-UNDER-ANY-TRUNK-AT-ALL-${far}\"")
        message(FATAL_ERROR "the far end of a given-back tap lost its connection")
    endif()
endforeach()

# --- a node the idiom declines still labels every pin ------------------------
# SW1 has four pins, so it is no two-terminal tap and UNCLAIMED has nothing to
# hang off a trunk. It stays exactly as it was: a label at every pin.
count_matches(un_labels "${html}" "class=\"netlabel\"[^>]*data-net=\"UNCLAIMED\"")
if(un_labels LESS 3)
    message(FATAL_ERROR "UNCLAIMED is claimed by no idiom, so each of its three "
                        "pins keeps a label; got ${un_labels}")
endif()

# --- the two-pin idioms still own the pairs ----------------------------------
# BUCK-BOOT joins exactly two pins and is no node's business: it stays a pair.
count_matches(bb_dots "${html}" "class=\"dot\"[^>]*data-net=\"BUCK-BOOT\"")
if(NOT bb_dots EQUAL 0)
    message(FATAL_ERROR "a two-pin net must never grow a junction dot")
endif()

message(STATUS "render nodes: trunks, taps, junction dots, the label fallback and "
               "determinism all verified")
