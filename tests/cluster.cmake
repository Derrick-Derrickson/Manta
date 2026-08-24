# The reference vocabulary, end to end: the cluster placer must draw the
# shapes a datasheet reference schematic is made of, and draw REPEATED ones
# identically. The fixture holds one of each:
#
#   - two IDENTICAL driver channels (Q1/R1/R2 and Q2/R3/R4, the same part
#     declarations wired to different nets -- ChugChug's PUMP-1/PUMP-2): the
#     clusters carry equal shapeKeys, take the metric union, and every member
#     part must sit at the same offset from its anchor in both;
#   - a buck-style artery (U1): the SW junction net with a catch diode to
#     ground, a bootstrap cap to a private far net, and the inductor inline
#     onto the output rail -- one horizontal trunk, wordless;
#   - a crystal pair (U2/Y1/C2/C3): two 3-pin nets around a two-terminal
#     part; both refuse to junction and route, the caps stand as satellites,
#     and the crystal stays beside its anchor;
#   - the pin-count rule: U1 drinks VBUS through two supply pins and earns a
#     pin-only rail segment; every rail bar stays group-width, never
#     room-wide;
#   - determinism (spec 15.8): two renders, identical bytes.
#
# CH2-INPUT is deliberately far wider than IN1: unaligned, the two channels'
# junction tap pitches would differ and the member offsets with them, so the
# identical-channel check is a live test of the metric union, not a tautology.
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

# The first refdes text position inside a symbol's <g> group. Every symbol
# prints one, at a geometry-fixed offset from the symbol origin, so DELTAS of
# refdes positions equal deltas of symbol positions.
function(sym_pos name out_x out_y)
    string(FIND "${html}" "<g class=\"sym\" data-c=\"${name}\">" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "symbol ${name} is not on the sheet")
    endif()
    string(SUBSTRING "${html}" ${pos} 2500 seg)
    if(NOT seg MATCHES "<text class=\"refdes\" x=\"(-?[0-9]+)\" y=\"(-?[0-9]+)\"")
        message(FATAL_ERROR "symbol ${name} carries no refdes text")
    endif()
    set(${out_x} ${CMAKE_MATCH_1} PARENT_SCOPE)
    set(${out_y} ${CMAKE_MATCH_2} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(SRC "${WORK}/cluster.manta")
file(WRITE "${SRC}" "\
netclass power { &CURRENT=1A; };

part FIX-NPN { @!type = npn; @~footprint = SOT-23; 1 : B; 2 : C; 3 : E; };
part FIX-RB { @!type = resistor; @~footprint = R-0603; #value = 1kR; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-RPD { @!type = resistor; @~footprint = R-0603; #value = 10kR; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-D { @!type = diode; @~footprint = SOD-123; 1 : A; 2 : K; };
part FIX-C { @!type = capacitor; @~footprint = C-0603; #value = 100nF; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-L { @!type = inductor; @~footprint = L-0805; #value = 4u7H; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-X { @!type = crystal; @~footprint = HC-49; #value = 8MHz; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-BUCK {
    @~footprint = SOIC-8;
    1 : VIN< &TYPE=POWER;
    2 : VIN2< &TYPE=POWER;
    3 : SW>;
    4 : BST;
    5 : EN<;
    6 : GND< &TYPE=POWER &~NET=GND;
};
part FIX-OSC {
    @~footprint = QFN-16;
    1 : XI;
    2 : XO;
    3 : D0>;
    4 : D1>;
    5 : VCC< &TYPE=POWER;
    6 : GND< &TYPE=POWER &~NET=GND;
};

block fixture {
    GND &TYPE=GROUND;
    VBUS &CLASS=power;
    VOUT &CLASS=power;
    3V3 &CLASS=power;

    --- CHANNELS
    {Q1~FIX-NPN: .B = BASE1; .C = LOAD1; .E = GND;};
    {R1~FIX-RB: .A = IN1; .B = BASE1;};
    {R2~FIX-RPD: .A = BASE1; .B = GND;};
    {Q2~FIX-NPN: .B = BASE2; .C = LOAD2; .E = GND;};
    {R3~FIX-RB: .A = CH2-INPUT; .B = BASE2;};
    {R4~FIX-RPD: .A = BASE2; .B = GND;};

    --- BUCK
    {U1~FIX-BUCK: .VIN = VBUS; .VIN2 = VBUS; .SW = SWNODE; .BST = BOOT; .EN = ENA;};
    {D1~FIX-D: .A = GND; .K = SWNODE;};
    {C1~FIX-C: .A = SWNODE; .B = BOOT;};
    {L1~FIX-L: .A = SWNODE; .B = VOUT;};

    --- CLOCK
    {U2~FIX-OSC: .XI = XTI; .XO = XTO; .VCC = 3V3; .D0 = DAT0; .D1 = DAT1;};
    {Y1~FIX-X: .A = XTI; .B = XTO;};
    {C2~FIX-C: .A = GND; .B = XTI;};
    {C3~FIX-C: .A = GND; .B = XTO;};
    3V3 = .{C4~FIX-C: . = GND;};
};
")

run_manta(compile -o "${WORK}/build/" "${SRC}")
run_manta(link --top fixture -L "${WORK}/build" --no-erc -o "${WORK}/cluster.mantaNets")

# --- determinism (spec 15.8): two renders, identical bytes -------------------
run_manta(render -o "${WORK}/cluster.html" "${WORK}/cluster.mantaNets")
run_manta(render -o "${WORK}/cluster2.html" "${WORK}/cluster.mantaNets")
file(SHA256 "${WORK}/cluster.html" a)
file(SHA256 "${WORK}/cluster2.html" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "a sheet of cluster compositions is not byte-reproducible")
endif()

file(READ "${WORK}/cluster.html" html)

# --- the artery is one horizontal trunk --------------------------------------
# Every horizontal wire segment of the SW junction net sits on ONE y: the
# lead-in at the pin's row, the trunk through the tap columns, the entry gap
# of the inline inductor. A second y would mean the artery broke into steps.
set(trunk_y "")
string(REGEX MATCHALL "<line class=\"wire\" x1=\"-?[0-9]+\" y1=\"-?[0-9]+\" x2=\"-?[0-9]+\" y2=\"-?[0-9]+\" data-net=\"SWNODE\"" sw_lines "${html}")
foreach(l ${sw_lines})
    string(REGEX REPLACE ".*y1=\"(-?[0-9]+)\" x2=\"-?[0-9]+\" y2=\"(-?[0-9]+)\".*" "\\1;\\2" ys "${l}")
    list(GET ys 0 y1)
    list(GET ys 1 y2)
    if(y1 EQUAL y2)
        if(trunk_y STREQUAL "")
            set(trunk_y ${y1})
        elseif(NOT y1 EQUAL trunk_y)
            message(FATAL_ERROR "SWNODE's trunk runs on two rows (${trunk_y} and ${y1}); "
                                "an artery is one horizontal wire")
        endif()
    endif()
endforeach()
string(REGEX MATCHALL "<polyline class=\"wire\" points=\"[^\"]+\" data-net=\"SWNODE\"" sw_polys "${html}")
foreach(pl ${sw_polys})
    string(REGEX MATCHALL "-?[0-9]+,-?[0-9]+" pts "${pl}")
    set(prev_y "")
    foreach(pt ${pts})
        string(REGEX REPLACE "-?[0-9]+,(-?[0-9]+)" "\\1" py "${pt}")
        if(NOT prev_y STREQUAL "" AND py EQUAL prev_y)
            if(trunk_y STREQUAL "")
                set(trunk_y ${py})
            elseif(NOT py EQUAL trunk_y)
                message(FATAL_ERROR "SWNODE's trunk runs on two rows (${trunk_y} and ${py})")
            endif()
        endif()
        set(prev_y ${py})
    endforeach()
endforeach()
if(trunk_y STREQUAL "")
    message(FATAL_ERROR "SWNODE has no horizontal trunk segment at all")
endif()

# --- identical channels draw identically -------------------------------------
# Q1/R1/R2 and Q2/R3/R4 are the same part declarations on different nets: the
# clusters share a shapeKey, so after the metric union every member must sit
# at the SAME offset from its anchor in both -- byte-identical channel cells.
sym_pos("Q1" q1x q1y)
sym_pos("R1" r1x r1y)
sym_pos("R2" r2x r2y)
sym_pos("Q2" q2x q2y)
sym_pos("R3" r3x r3y)
sym_pos("R4" r4x r4y)
foreach(pair "r1x;r1y;r3x;r3y;R1/R3" "r2x;r2y;r4x;r4y;R2/R4")
    list(GET pair 0 ax)
    list(GET pair 1 ay)
    list(GET pair 2 bx)
    list(GET pair 3 by)
    list(GET pair 4 tag)
    math(EXPR dax "${${ax}} - ${q1x}")
    math(EXPR day "${${ay}} - ${q1y}")
    math(EXPR dbx "${${bx}} - ${q2x}")
    math(EXPR dby "${${by}} - ${q2y}")
    if(NOT dax EQUAL dbx OR NOT day EQUAL dby)
        message(FATAL_ERROR "identical channels differ: ${tag} offsets are "
                            "(${dax},${day}) vs (${dbx},${dby}); equal-shapeKey "
                            "clusters must lay out byte-identically")
    endif()
endforeach()
# Adjacent packing of identical cells: side by side on one row (equal body
# rows) or stacked in one column (equal x) -- never scattered diagonally.
if(NOT q1x EQUAL q2x AND NOT q1y EQUAL q2y)
    message(FATAL_ERROR "the identical channels neither share a row nor a column: "
                        "equal-shapeKey clusters must pack adjacently")
endif()

# --- the pin-count rule and group-width rail segments ------------------------
# U1's two VIN pins alone earn VBUS a pin-only bar; and every bar on the
# sheet spans less than half its room -- the room-wide bars the redesign
# abolished would fail immediately.
if(NOT html MATCHES "class=\"railbar\"[^>]*data-net=\"VBUS\"")
    message(FATAL_ERROR "U1's two supply pins must earn VBUS a pin-only rail bar")
endif()
string(REGEX MATCHALL "<line class=\"railbar\" x1=\"-?[0-9]+\" y1=\"-?[0-9]+\" x2=\"-?[0-9]+\"" bar_geoms "${html}")
string(REGEX MATCHALL "<rect class=\"room\" x=\"-?[0-9]+\" y=\"-?[0-9]+\" width=\"[0-9]+\" height=\"[0-9]+\"" room_geoms "${html}")
foreach(bar ${bar_geoms})
    string(REGEX REPLACE ".*x1=\"(-?[0-9]+)\" y1=\"(-?[0-9]+)\" x2=\"(-?[0-9]+)\".*" "\\1;\\2;\\3" bxy "${bar}")
    list(GET bxy 0 bx1)
    list(GET bxy 1 by)
    list(GET bxy 2 bx2)
    math(EXPR bar_w "${bx2} - ${bx1}")
    set(contained FALSE)
    foreach(rm ${room_geoms})
        string(REGEX REPLACE ".*x=\"(-?[0-9]+)\" y=\"(-?[0-9]+)\" width=\"([0-9]+)\" height=\"([0-9]+)\".*" "\\1;\\2;\\3;\\4" rxy "${rm}")
        list(GET rxy 0 rx)
        list(GET rxy 1 ry)
        list(GET rxy 2 rw)
        list(GET rxy 3 rh)
        math(EXPR rx2 "${rx} + ${rw}")
        math(EXPR ry2 "${ry} + ${rh}")
        if(bx1 GREATER_EQUAL rx AND bx2 LESS_EQUAL rx2 AND by GREATER_EQUAL ry AND by LESS_EQUAL ry2)
            set(contained TRUE)
            math(EXPR half_w "${rw} / 2")
            if(NOT bar_w LESS half_w)
                message(FATAL_ERROR "rail bar spans ${bar_w} of a ${rw}-wide room: bars "
                                    "must stay group-width, under half the room")
            endif()
        endif()
    endforeach()
    if(NOT contained)
        message(FATAL_ERROR "a rail bar lies in no room at all")
    endif()
endforeach()

# --- chain interiors are wordless --------------------------------------------
# BASE1/BASE2 (the channel junctions) and SWNODE (the buck junction) are
# fully drawn conductors: wires yes, labels never. BOOT stays Free and keeps
# its paired labels -- that is the private-far-net idiom, not tested here.
foreach(net "BASE1" "BASE2" "SWNODE")
    if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"${net}\"")
        message(FATAL_ERROR "${net} is drawn whole inside its cluster and must carry no label")
    endif()
    if(NOT html MATCHES "class=\"wire\"[^>]*data-net=\"${net}\"")
        message(FATAL_ERROR "${net} must be drawn as a wire")
    endif()
endforeach()

# --- the crystal stays beside its anchor -------------------------------------
# XTI/XTO refuse to junction (the crystal bridges them) and route instead;
# the caps stand as satellites and the crystal keeps its own little run --
# which must land within 6 pin pitches of U2's body, not across the room.
string(FIND "${html}" "<g class=\"sym\" data-c=\"U2\">" u2pos)
if(u2pos EQUAL -1)
    message(FATAL_ERROR "U2 is not on the sheet")
endif()
string(SUBSTRING "${html}" ${u2pos} 2500 u2seg)
if(NOT u2seg MATCHES "<rect class=\"body\" x=\"(-?[0-9]+)\" y=\"-?[0-9]+\" width=\"([0-9]+)\"")
    message(FATAL_ERROR "U2 has no drawn body rectangle")
endif()
set(u2x ${CMAKE_MATCH_1})
set(u2w ${CMAKE_MATCH_2})
sym_pos("Y1" y1x y1y)
math(EXPR lo "${u2x} - 60")
math(EXPR hi "${u2x} + ${u2w} + 60")
if(y1x LESS lo OR y1x GREATER hi)
    message(FATAL_ERROR "the crystal sits at x=${y1x}, outside 60 units of its "
                        "anchor's body [${u2x}, ${u2x}+${u2w}]: crystals belong "
                        "beside their IC")
endif()
# Both routed crystal nets are wordless conductors too.
foreach(net "XTI" "XTO")
    if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"${net}\"")
        message(FATAL_ERROR "${net} routes whole and must carry no label")
    endif()
    if(NOT html MATCHES "class=\"wire\"[^>]*data-net=\"${net}\"")
        message(FATAL_ERROR "${net} must be drawn as a wire")
    endif()
endforeach()

message(STATUS "render cluster: identical channels byte-aligned, single-y artery, "
               "group-width rail bars, wordless chains, crystal in place, "
               "determinism -- all verified")
