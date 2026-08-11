# The layout engine, end to end through the real binary: rooms from section
# markers, a rail bar with its decoupling ladder, pull-ups and a chain on an
# anchor, port flags where a net crosses rooms, multi-page hierarchy -- one
# page per block definition, sheet symbols on the parent page -- and the
# determinism guarantee of spec 15.8 over the whole HTML.
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
# the untitled room exists too. LEFTY[0] and SPI-CLK deliberately span rooms.
set(SRC "${WORK}/rooms.manta")
file(WRITE "${SRC}" "\
netclass power { &CURRENT=1A; };

netclass raw-power { &CURRENT=2A; };

part FIX-R { @!type = resistor; @~footprint = R-0603; #value = 10kR; 1 = A &CASUAL; 2 = B &CASUAL; };
part FIX-C { @!type = capacitor; @~footprint = C-0603; #value = 100nF; 1 = A &CASUAL; 2 = B &CASUAL; };
part FIX-LED { @!type = led; @~footprint = R-0603; #value = red; 1 = A; 2 = K; };
part FIX-TP { @!type = testpoint; @~footprint = TP-1MM; 1 = T; };
part FIX-MCU {
    @~footprint = QFP-STM32-32;
    1 = VCC< &TYPE=POWER;
    2 = GND< &TYPE=POWER &~NET=GND;
    3 = RST<;
    4 = IO0>;
    5 = IO1>;
    6 = AVDD< &TYPE=POWER;
};
part FIX-NC {
    @~footprint = QFN-4;
    1 = IN;
    2 = OUT;
    3 = SPARE &TYPE=NC;
    4 = SPARE2 &TYPE=NC;
};
part FIX-HDR {
    @!type = boardconnector;
    @~footprint = HDR-1x4;
    1 = A &CASUAL;
    2 = B &CASUAL;
    3 = C &CASUAL;
    4 = D &CASUAL;
};

block sub {
    >DRIVE;
    >>GND;
    DRIVE = .{R1~FIX-R}. = INNER-NODE;
    INNER-NODE = A{D1~FIX-LED}K = GND;
};

block fixture {
    GND &TYPE=GROUND;
    3V3 &CLASS=power;
    RAWPWR &CLASS=raw-power &STUB;
    PROBE &STUB;
    

    {U9~FIX-R: A = LEFTY[0]; B = LEFTY[1];};

    --- MCU CORE
    {U1~FIX-MCU: VCC = 3V3; RST = nRST; IO0 = LED-A; IO1 = SPI-CLK; AVDD = VBAT;};
    3V3 = .{R1~FIX-R}. == nRST;
    LED-A = .{R2~FIX-R}. = LED-K;
    LED-K = A{D1~FIX-LED}K = GND;
    3V3 == .{C1~FIX-C: . = GND;};
    3V3 == .{C2~FIX-C: . = GND;};
    VBAT == .{C3~FIX-C: . = GND;};
    VBAT == .{C4~FIX-C: . = GND;};

    --- IO HEADER
    {J1~FIX-HDR: A = SPI-CLK; B = GND; C = VBAT; D = EXTRA;};

    --- MISC
    {R7~FIX-R: A = LEFTY[0]; B = LEFTY[1];};
    {R8~FIX-R: A = LEFTY[0]; B = LEFTY[1];};
    {TP1~FIX-TP: T = SPI-CLK;};
    {TP2~FIX-TP: T = PROBE;};
    {TP3~FIX-TP: T = RAWPWR;};
    {U4~FIX-NC: IN = SPI-CLK; OUT = LED-A; SPARE = DEAD-PIN;};

    --- SUBS
    DRV[0:1] = [[{SUB%[1:2]~sub}DRIVE]];
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

# --- TEMPORARY (WP7 removes this): the pipeline flag -------------------------
# '--layout=flow' selects the Flow pipeline, whose stub delegates to the
# classic engine, so its output must be byte-identical to the default. This
# assertion proves the flag and the delegation; it dies when the flow placer
# lands and the outputs legitimately diverge.
run_manta(render --layout=flow -o "${WORK}/fixture-flow.html" "${WORK}/fixture.mantaNets")
file(SHA256 "${WORK}/fixture-flow.html" f)
if(NOT a STREQUAL f)
    message(FATAL_ERROR "--layout=flow must render byte-identically to the default "
                        "while its stub delegates to the classic engine")
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

# --- a rail bar per rail with >= 2 ladder caps -------------------------------
string(REGEX MATCHALL "class=\"railbar\"" bars "${html}")
list(LENGTH bars bar_count)
if(NOT bar_count EQUAL 2)
    message(FATAL_ERROR "expected rail bars for 3V3 and VBAT, got ${bar_count}")
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

# --- the drawn idioms actually drew ------------------------------------------
# The GPIO chain's LED-K net lives entirely inside one run: wire, never label.
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"LED-K\"")
    message(FATAL_ERROR "LED-K is drawn inside a chain and must not be labelled")
endif()
if(NOT html MATCHES "data-net=\"LED-K\"")
    message(FATAL_ERROR "the LED chain did not draw the LED-K wire")
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

message(STATUS "render: rooms, rail bars, chains, port flags and block pages all verified")
