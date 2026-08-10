# The layout engine, end to end through the real binary: rooms from section
# markers, a rail bar with its decoupling ladder, pull-ups and a chain on an
# anchor, port flags where a net crosses rooms, and the determinism guarantee
# of spec 15.8 over the whole HTML.
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
part FIX-HDR {
    @!type = boardconnector;
    @~footprint = HDR-1x4;
    1 = A &CASUAL;
    2 = B &CASUAL;
    3 = C &CASUAL;
    4 = D &CASUAL;
};

block fixture {
    GND &TYPE=GROUND;
    3V3 &CLASS=power;
    

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

# --- a rail bar per rail with >= 2 ladder caps -------------------------------
string(REGEX MATCHALL "class=\"railbar\"" bars "${html}")
list(LENGTH bars bar_count)
if(NOT bar_count EQUAL 2)
    message(FATAL_ERROR "expected rail bars for 3V3 and VBAT, got ${bar_count}")
endif()

# --- port flags: SPI-CLK spans the MCU, IO and MISC rooms --------------------
string(REGEX MATCHALL "class=\"portflag\" data-net=\"SPI-CLK\"" flags "${html}")
list(LENGTH flags flag_count)
if(flag_count LESS 2)
    message(FATAL_ERROR "SPI-CLK crosses rooms and must appear as a port flag in "
                        "each, got ${flag_count}")
endif()

# --- the drawn idioms actually drew ------------------------------------------
# The GPIO chain's LED-K net lives entirely inside one run: wire, never label.
if(html MATCHES "class=\"netlabel\"[^>]*data-net=\"LED-K\"")
    message(FATAL_ERROR "LED-K is drawn inside a chain and must not be labelled")
endif()
if(NOT html MATCHES "data-net=\"LED-K\"")
    message(FATAL_ERROR "the LED chain did not draw the LED-K wire")
endif()

message(STATUS "render: rooms, rail bars, chains and port flags all verified")
