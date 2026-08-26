# The layout engine, end to end through the real binary: rooms from section
# markers, rail bars with taps and decoupling ladders, routed room-local nets,
# plain labels where a net crosses rooms, port flags only at block ports,
# multi-page hierarchy -- one page per block definition, sheet symbols on the
# parent page -- and the determinism guarantee of spec 15.8 over the whole
# HTML.
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

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# One block, three '--- TITLE' markers, and a part placed before any marker so
# the untitled room exists too. LEFTY[0] and SPI-CLK deliberately span rooms;
# nRST joins two anchors (U1, J2) and a pull-up inside one room, so the
# placer has a room-local net to route.
set(SRC "${WORK}/rooms.manta")
file(WRITE "${SRC}" "\
netclass power { &CURRENT=1A; };

netclass raw-power { &CURRENT=2A; };

part FIX-R { @!type = resistor; @~footprint = R-0603; #value = 10kR; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-C { @!type = capacitor; @~footprint = C-0603; #value = 100nF; 1 : A &CASUAL; 2 : B &CASUAL; };
part FIX-LED { @!type = led; @~footprint = R-0603; #value = red; 1 : A; 2 : K; };
part FIX-TP { @!type = testpoint; @~footprint = TP-1MM; 1 : T; };
part FIX-MCU {
    @~footprint = QFP-STM32-32;
    1 : VCC< &TYPE=POWER;
    2 : GND< &TYPE=POWER;
    3 : RST<;
    4 : IO0>;
    5 : IO1>;
    6 : AVDD< &TYPE=POWER;
};
part FIX-NC {
    @~footprint = QFN-4;
    1 : IN;
    2 : OUT;
    3 : SPARE &TYPE=NC;
    4 : SPARE2 &TYPE=NC;
};
part FIX-HDR {
    @!type = boardconnector;
    @~footprint = HDR-1x4;
    1 : A &CASUAL;
    2 : B &CASUAL;
    3 : C &CASUAL;
    4 : D &CASUAL;
};
part FIX-DUAL {
    @~footprint = QFN-8;
    1 : VA< &TYPE=POWER;
    2 : VB< &TYPE=POWER;
    3 : IO0>;
    4 : IO1<;
};

block sub {
    >DRIVE;
    >>GND;
    DRIVE = .{R1~FIX-R}. = INNER-NODE;
    INNER-NODE = A.{D1~FIX-LED}.K = GND;
};

block fixture {
    GND &TYPE=GROUND;
    3V3 &CLASS=power;
    RAWPWR &CLASS=raw-power &STUB;
    RAILX &CLASS=power;
    PROBE &STUB;
    PG-OUT &STUB;


    {U9~FIX-R: .A = LEFTY[0]; .B = LEFTY[1];};

    --- MCU CORE
    {U1~FIX-MCU: .VCC = 3V3; .RST = nRST; .IO0 = LED-A; .IO1 = SPI-CLK; .AVDD = VBAT; .GND = GND;};
    {J2~FIX-HDR: .A = nRST;};
    3V3 = .{R1~FIX-R}. = nRST;
    LED-A = .{R2~FIX-R}. = LED-K;
    LED-K = A.{D1~FIX-LED}.K = GND;
    3V3 = .{C1~FIX-C: . = GND;};
    3V3 = .{C2~FIX-C: . = GND;};
    VBAT = .{C3~FIX-C: . = GND;};
    VBAT = .{C4~FIX-C: . = GND;};

    --- IO HEADER
    {J1~FIX-HDR: .A = SPI-CLK; .B = GND; .C = VBAT; .D = EXTRA;};

    --- PIN GROUP
    {U5~FIX-DUAL: .VA = RAILX; .VB = RAILX; .IO0 = PG-OUT;};

    --- MISC
    {R7~FIX-R: .A = LEFTY[0]; .B = LEFTY[1];};
    {R8~FIX-R: .A = LEFTY[0]; .B = LEFTY[1];};
    {TP1~FIX-TP: .T = SPI-CLK;};
    {TP2~FIX-TP: .T = PROBE;};
    {TP3~FIX-TP: .T = RAWPWR;};
    {U4~FIX-NC: .IN = SPI-CLK; .OUT = LED-A; .SPARE = DEAD-PIN;};

    --- SUBS
    DRV[0:1] = [[{SUB%[1:2]~sub}.DRIVE]];
};
")

run_manta(compile -o "${WORK}/build/" "${SRC}")
run_manta(link --top fixture -L "${WORK}/build" --no-erc -o "${WORK}/fixture.mantaNets")

# --- determinism (spec 15.8): two renders, identical bytes -------------------
run_manta(render -o "${WORK}/fixture.html" "${WORK}/fixture.mantaNets")
run_manta(render -o "${WORK}/fixture2.html" "${WORK}/fixture.mantaNets")
file(SHA256 "${WORK}/fixture.html" a)
file(SHA256 "${WORK}/fixture2.html" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "render is not deterministic")
endif()

file(READ "${WORK}/fixture.html" html)

# --- rooms: three titled, plus the untitled one for U9 -----------------------
foreach(title "MCU CORE" "IO HEADER" "MISC")
    if(NOT html MATCHES "class=\"roomtitle\"[^>]*>${title}<")
        message(FATAL_ERROR "the rendered sheet has no room titled '${title}'")
    endif()
endforeach()
string(REGEX MATCHALL "class=\"room\"" room_rects "${html}")
list(LENGTH room_rects room_count)
if(room_count LESS 4)
    message(FATAL_ERROR "expected 4 framed rooms (3 titled + 1 untitled), got ${room_count}")
endif()

# --- rail bars ----------------------------------------------------------------
# A rail whose consumer group lives in a cluster gets one bar there: 3V3
# (group C1,C2) and VBAT (group C3,C4), both in U1's cluster in MCU CORE, and
# RAILX in U5's cluster in PIN GROUP -- earned by the anchor's own two supply
# pins alone, the pin-count rule with no ladder at all. RAWPWR has a single
# pin anywhere (TP3) and never earns one.
string(REGEX MATCHALL "class=\"railbar\"" bars "${html}")
list(LENGTH bars bar_count)
if(NOT bar_count EQUAL 3)
    message(FATAL_ERROR "expected rail bars for 3V3, VBAT and RAILX, got ${bar_count}")
endif()

# --- the pin-count rule, end to end ------------------------------------------
# U5 drinks RAILX through two supply pins and nothing else touches the rail:
# the cluster earns a pin-only segment, both pins tap it, and not one
# per-part rail flag survives anywhere on RAILX.
if(NOT html MATCHES "class=\"railbar\"[^>]*data-net=\"RAILX\"")
    message(FATAL_ERROR "U5's two supply pins must earn RAILX a pin-only rail bar")
endif()
string(REGEX MATCHALL "class=\"rail\" data-net=\"RAILX\"" railx_marks "${html}")
list(LENGTH railx_marks railx_mark_count)
if(NOT railx_mark_count EQUAL 0)
    message(FATAL_ERROR "both RAILX pins tap their cluster's segment, so no rail "
                        "flag may remain; got ${railx_mark_count}")
endif()

# --- rail bars are LOCAL: group-width segments, never room-wide -------------
# The cluster placer draws one short segment per decap group, widened only to
# the cluster's own tap columns. Each bar must sit inside a room and span
# less than half that room's width -- the room-wide bars the redesign
# abolished would fail this immediately.
string(REGEX MATCHALL "<line class=\"railbar\" x1=\"[0-9-]+\" y1=\"[0-9-]+\" x2=\"[0-9-]+\"" bar_geoms "${html}")
string(REGEX MATCHALL "<rect class=\"room\" x=\"[0-9-]+\" y=\"[0-9-]+\" width=\"[0-9]+\" height=\"[0-9]+\"" room_geoms "${html}")
foreach(bar ${bar_geoms})
    string(REGEX REPLACE ".*x1=\"([0-9-]+)\" y1=\"([0-9-]+)\" x2=\"([0-9-]+)\".*" "\\1;\\2;\\3" bxy "${bar}")
    list(GET bxy 0 bx1)
    list(GET bxy 1 by)
    list(GET bxy 2 bx2)
    math(EXPR bar_w "${bx2} - ${bx1}")
    set(contained FALSE)
    foreach(rm ${room_geoms})
        string(REGEX REPLACE ".*x=\"([0-9-]+)\" y=\"([0-9-]+)\" width=\"([0-9]+)\" height=\"([0-9]+)\".*" "\\1;\\2;\\3;\\4" rxy "${rm}")
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

# --- a room-local net is a routed wire, not repeated labels -------------------
# nRST joins U1.RST, J2.A and pull-up R1, all inside MCU CORE: the router
# claims it, so its data-net rides wire elements (the routed tree and the
# stubs) and not one netlabel appears for it anywhere.
if(NOT html MATCHES "class=\"wire\"[^>]*data-net=\"nRST\"")
    message(FATAL_ERROR "the room-local net nRST must be drawn as a wire")
endif()
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"nRST\"")
    message(FATAL_ERROR "a routed net must not carry a label")
endif()

# --- one rail segment per group, per-part flags only outside it --------------
# VBAT has three consumer pins in MCU CORE (C3, C4, U1.AVDD) and one in
# IO HEADER (J1.C). The MCU CORE appearance is the group's bar -- named once
# at its left end -- with every cluster consumer tapping it, so the only
# rail-flag mark VBAT keeps is J1.C's per-part flag in IO HEADER: exactly 1,
# never one per pin (4).
if(NOT html MATCHES "class=\"railbar\"[^>]*data-net=\"VBAT\"")
    message(FATAL_ERROR "VBAT has 3 consumer pins in MCU CORE and must get a rail bar")
endif()
string(REGEX MATCHALL "class=\"rail\" data-net=\"VBAT\"" vbat_marks "${html}")
list(LENGTH vbat_marks vbat_mark_count)
if(NOT vbat_mark_count EQUAL 1)
    message(FATAL_ERROR "VBAT's cluster consumers tap the bar and only J1.C keeps a "
                        "flag, so exactly 1 rail-flag mark; got ${vbat_mark_count}")
endif()

# --- room-crossing nets: a plain label, not a port flag ----------------------
# SPI-CLK spans the MCU, IO and MISC rooms. Crossing a room boundary is not
# crossing a page: only a block-port net (direction != None) earns the flag,
# so SPI-CLK labels each appearance and flies no flag anywhere.
string(REGEX MATCHALL "class=\"netlabel\"[^>]*data-net=\"SPI-CLK\"" labels "${html}")
list(LENGTH labels label_count)
if(label_count LESS 2)
    message(FATAL_ERROR "SPI-CLK crosses rooms and must appear as a plain label in "
                        "each, got ${label_count}")
endif()
if(html MATCHES "class=\"portflag\" data-net=\"SPI-CLK\"")
    message(FATAL_ERROR "a merely room-crossing net must not fly a port flag")
endif()

# --- rail classification by CLASS token --------------------------------------
# A CLASS value containing the token 'power' ('raw-power') is a rail, so
# RAWPWR renders as a rail flag, never a plain label.
if(NOT html MATCHES "class=\"rail\" data-net=\"RAWPWR\"")
    message(FATAL_ERROR "a net of class 'raw-power' must render a rail flag")
endif()
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"RAWPWR\"")
    message(FATAL_ERROR "a rail net must not carry a plain label")
endif()

# --- the LED string's interior net is drawn, never labelled -------------------
# LED-K joins R2 and D1 only. Whether it rides inside a series string or the
# router joins the pair, it is a private room-local net: a drawn conductor
# with no label anywhere -- the same guarantee the classic chain idiom gave.
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"LED-K\"")
    message(FATAL_ERROR "LED-K is room-local and private and must not be labelled")
endif()
if(NOT html MATCHES "data-net=\"LED-K\"")
    message(FATAL_ERROR "the LED string did not draw the LED-K wire")
endif()

# --- no-connect pins: a cross, never a net label ------------------------------
# Spec 11.6: '&TYPE=NC' forbids connection, so U4's SPARE and SPARE2 are drawn
# as crosses -- SPARE despite carrying the net DEAD-PIN, SPARE2 despite
# carrying no net at all. Two crosses of two strokes each.
string(REGEX MATCHALL "class=\"noconn\"" nc_strokes "${html}")
list(LENGTH nc_strokes nc_stroke_count)
if(NOT nc_stroke_count EQUAL 4)
    message(FATAL_ERROR "expected 2 no-connect crosses (4 strokes) for U4's NC pins, "
                        "got ${nc_stroke_count} strokes")
endif()

# The net a NC pin happens to carry names no conductor: it must not be drawn.
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"DEAD-PIN\"")
    message(FATAL_ERROR "a NC pin's net must not be labelled")
endif()
if(html MATCHES ">DEAD-PIN<")
    message(FATAL_ERROR "the NC pin's net name is drawn as text somewhere")
endif()

# The cross sits WITH the existing greyed pin text, not instead of it.
if(NOT html MATCHES "class=\"pinname nc\"")
    message(FATAL_ERROR "a NC pin's name must still be drawn greyed")
endif()

# A single-pin net that is NOT typed NC keeps its label: '&STUB' declares the
# single reference deliberate (spec 11.8), so PROBE is a named test point the
# reader wants to see, not noise. Only '&TYPE=NC' means "do not connect".
if(NOT html MATCHES "class=\"netlabel\"[^>]*data-net=\"PROBE\"")
    message(FATAL_ERROR "a deliberate single-pin stub net must still be labelled")
endif()

# --- wires dominate labels ----------------------------------------------------
# Every placed pin has at least its stub wire and the router turns label nets
# into wires, so wire elements must be at least as numerous as netlabel texts.
# Parity is a defensible floor on this small fixture: below it, half the sheet
# would connect by name alone, which is exactly what the flow placer exists to
# avoid.
string(REGEX MATCHALL "class=\"wire\"" all_wires "${html}")
string(REGEX MATCHALL "class=\"netlabel\"" all_labels "${html}")
list(LENGTH all_wires wire_count)
list(LENGTH all_labels all_label_count)
if(wire_count LESS all_label_count)
    message(FATAL_ERROR "output has ${wire_count} wires vs ${all_label_count} netlabels; "
                        "wires must dominate")
endif()

# --- hierarchy: one page per block DEFINITION --------------------------------
# 'sub' is instantiated twice but renders once: SUB2 gets no page of its own.
string(REGEX MATCHALL "id=\"page-sub\"" sub_pages "${html}")
list(LENGTH sub_pages sub_page_count)
if(NOT sub_page_count EQUAL 1)
    message(FATAL_ERROR "expected exactly one page for block 'sub', got ${sub_page_count}")
endif()

# Two green sheet symbols on the top page, each an anchor to the shared page;
# the third href is the sidebar's entry for the page itself.
string(REGEX MATCHALL "class=\"sbody\"" sheet_syms "${html}")
list(LENGTH sheet_syms sheet_sym_count)
if(NOT sheet_sym_count EQUAL 2)
    message(FATAL_ERROR "expected 2 sheet symbols for SUB1 and SUB2, got ${sheet_sym_count}")
endif()
string(REGEX MATCHALL "href=\"#page-sub\"" sub_links "${html}")
list(LENGTH sub_links sub_link_count)
if(NOT sub_link_count EQUAL 3)
    message(FATAL_ERROR "expected 3 links to page-sub (2 sheet symbols + sidebar), "
                        "got ${sub_link_count}")
endif()

# The definition page labels by LOCAL spelling: the DRIVE port renders as a
# port flag whose data-net is still the flat design-wide name DRV[0].
if(NOT html MATCHES "class=\"portflag\" data-net=\"DRV\\[0\\]\"")
    message(FATAL_ERROR "the sub page has no DRIVE port flag on net DRV[0]")
endif()
if(NOT html MATCHES ">DRIVE<")
    message(FATAL_ERROR "the local port spelling DRIVE is drawn nowhere")
endif()

# The chain inside 'sub' draws its private net once, under the flat name.
if(NOT html MATCHES "data-net=\"SUB1.INNER-NODE\"")
    message(FATAL_ERROR "the sub page's chain wire does not carry the flat net name")
endif()
if(html MATCHES "data-net=\"SUB2.INNER-NODE\"")
    message(FATAL_ERROR "SUB2's copy rendered: the 2nd instance must draw no page")
endif()

message(STATUS "render: rooms, rail bars, routed nets, labels, port flags, block "
               "pages and determinism all verified")

# --- '&RENDER' (spec 11.3, revision 1.6) -------------------------------------
# WIRE must draw copper, never a name; LABEL must name, never route; and a
# WIRE net that cannot be drawn -- here one spanning two rooms -- falls back
# to names and says so with W-RENDER.
set(RSRC "${WORK}/rmode.manta")
file(WRITE "${RSRC}" "\
netclass power { &CURRENT=1A; };

part RM-R { @~footprint = R-0603; 1 : A &CASUAL; 2 : B &CASUAL; };
part RM-TP { @~footprint = TP-1MM; 1 : T; };

block rmode {
    GND &TYPE=GROUND;

    // The rail heuristics would flag RAILY at every pin; WIRE overrides them.
    RAILY &CLASS=power &RENDER=WIRE;
    {R1~RM-R: .A = RAILY; .B = GND;};
    {T1~RM-TP: .T = RAILY;};

    // The router would draw MID wordlessly; LABEL forces the name.
    MID &RENDER=LABEL;
    X = .{R2~RM-R}. = MID;
    MID = .{R3~RM-R}. = GND;
    X = .{R4~RM-R}. = GND;

    FARWIRE &RENDER=WIRE;
    --- HERE
    {T2~RM-TP: .T = FARWIRE;};
    --- THERE
    {T3~RM-TP: .T = FARWIRE;};
};
")
run_manta(compile -o "${WORK}/rmodebuild/" "${RSRC}")
run_manta(link --top rmode -L "${WORK}/rmodebuild" --no-erc -o "${WORK}/rmode.mantaNets")
execute_process(COMMAND "${MANTA}" render -o "${WORK}/rmode.html" "${WORK}/rmode.mantaNets"
                RESULT_VARIABLE rmode_code OUTPUT_VARIABLE rmode_out ERROR_VARIABLE rmode_err)
if(NOT rmode_code EQUAL 0)
    message(FATAL_ERROR "render rmode failed:\n${rmode_out}${rmode_err}")
endif()
file(READ "${WORK}/rmode.html" rhtml)

# WIRE: RAILY is drawn -- at least one wire carries it -- and never named.
if(NOT rhtml MATCHES "class=\"wire\"[^>]*data-net=\"RAILY\"")
    message(FATAL_ERROR "'&RENDER=WIRE' net RAILY has no drawn wire")
endif()
if(rhtml MATCHES "class=\"netlabel\"[^>]*data-net=\"RAILY\"")
    message(FATAL_ERROR "'&RENDER=WIRE' net RAILY still shows a name")
endif()
if(rhtml MATCHES "class=\"railbar\"[^>]*data-net=\"RAILY\"")
    message(FATAL_ERROR "'&RENDER=WIRE' net RAILY still earned a rail bar")
endif()

# LABEL: MID shows its name.
if(NOT rhtml MATCHES "class=\"netlabel\"[^>]*data-net=\"MID\"")
    message(FATAL_ERROR "'&RENDER=LABEL' net MID shows no name")
endif()

# The cross-room WIRE net degraded to names, and the renderer said so.
if(NOT rmode_err MATCHES "W-RENDER")
    message(FATAL_ERROR "no W-RENDER warning for the cross-room FARWIRE:\n${rmode_err}")
endif()
if(NOT rmode_err MATCHES "FARWIRE")
    message(FATAL_ERROR "the W-RENDER warning does not name FARWIRE:\n${rmode_err}")
endif()
