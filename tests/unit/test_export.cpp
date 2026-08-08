// Export: footprint resolution, component identity and the KiCad emitter.
//
// The UUID and hash tests check against published vectors rather than against
// manta's own output. That distinction matters: a self-referential test would
// pass just as happily on a broken implementation, and the whole point of a
// name-based UUID is that a third party can recompute it.
#include <string>

#include "base/sha1.h"
#include "diag/engine.h"
#include "export/exporters.h"
#include "export/footprint_map.h"
#include "export/uuid.h"
#include "harness.h"

using namespace manta;

namespace {

// A design with one component and one net, enough to exercise the emitter.
Design tinyDesign() {
    Design design;
    design.top = "board";

    Component u1;
    // A component's path excludes the top block: one sitting directly in it is
    // just its own designator, and each level of nesting prepends a block.
    u1.path = {"U1"};
    u1.designator = "U1";
    u1.identity = "U1";
    u1.partName = "LDO-3V3";
    u1.footprint = "SOT-23-5";
    u1.fields.emplace_back("value", "AP2112K-3.3");

    ComponentPin vout;
    vout.physical = "5";
    vout.logical = "VOUT";
    vout.type = PinType::Power;
    vout.direction = PortDir::Out;
    vout.net = 0;
    u1.pins.push_back(vout);

    design.components.push_back(std::move(u1));

    Net net;
    net.name = "3V3";
    net.pins.push_back(PinRef{0, 0});
    design.nets.push_back(std::move(net));
    return design;
}

std::string exportOf(const Design& design, const ExportOptions& options, std::size_t* warnings) {
    SourceManager sources;
    DiagEngine diags(sources);
    std::string out = exportDesign(design, ExportFormat::KiCad, options, diags);
    if (warnings) *warnings = diags.warningCount();
    return out;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-1, against FIPS 180-4
// ---------------------------------------------------------------------------

TEST_CASE("sha1 matches the published vectors") {
    CHECK_EQ(sha1Hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(sha1Hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

TEST_CASE("sha1 pads correctly either side of the block boundary") {
    // 55 bytes fits the length field in one block; 56 forces a second. Getting
    // this wrong is the classic SHA-1 bug and it is invisible on short inputs.
    CHECK_EQ(sha1Hex(std::string(55, 'a')),
             std::string("c1c8bbdc22796e28c0e15163d20899b65621d65a"));
    CHECK_EQ(sha1Hex(std::string(56, 'a')),
             std::string("c2db330f6083854c99d4b5bfb6e8f29f201be699"));
    CHECK_EQ(sha1Hex(std::string(1000000, 'a')),
             std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

// ---------------------------------------------------------------------------
// UUIDs, against RFC 4122
// ---------------------------------------------------------------------------

TEST_CASE("uuidV5 matches the standard namespaces") {
    constexpr UuidBytes kDns = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
                                0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    constexpr UuidBytes kUrl = {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
                                0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};

    CHECK_EQ(formatUuid(uuidV5(kDns, "python.org")),
             std::string("886313e1-3b8a-5372-9b90-0c9aee199e5d"));
    CHECK_EQ(formatUuid(uuidV5(kDns, "example.com")),
             std::string("cfbff0d1-9375-5685-968c-48ce8b15ae17"));
    CHECK_EQ(formatUuid(uuidV5(kUrl, "http://example.com/")),
             std::string("0a300ee9-f9e4-5697-a51a-efc7fafaba67"));
}

TEST_CASE("a uuid carries its version and variant") {
    std::string uuid = formatUuid(uuidV5(kMantaNamespace, "board/U1"));
    CHECK_EQ(uuid.size(), std::size_t{36});
    CHECK_EQ(uuid[14], '5');  // version 5
    // Variant 10xx: one of 8, 9, a, b.
    CHECK(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');
}

TEST_CASE("a component's identity follows its path, not its designator") {
    // The whole reason for emitting a UUID: re-annotating must not move a
    // placed footprint, and moving a part between blocks must.
    std::vector<std::string> underPower = {"power", "U1"};
    std::vector<std::string> underSignal = {"signal", "U1"};

    CHECK_EQ(pathUuid(underPower, 2), pathUuid(underPower, 2));
    CHECK(pathUuid(underPower, 2) != pathUuid(underSignal, 2));
    // A prefix is the enclosing sheet, and is not the component's own id.
    CHECK(pathUuid(underPower, 1) != pathUuid(underPower, 2));
}

// ---------------------------------------------------------------------------
// The footprint map
// ---------------------------------------------------------------------------

namespace {

bool loadMap(std::string_view text, FootprintMap& out, std::size_t* errors = nullptr) {
    SourceManager sources;
    DiagEngine diags(sources);
    bool ok = loadFootprintMap(text, "test.fpmap", diags, out);
    if (errors) *errors = diags.errorCount();
    return ok;
}

}  // namespace

TEST_CASE("a map file parses entries, comments and blank lines") {
    FootprintMap map;
    CHECK(loadMap(R"(# a comment
R-0603     Resistor_SMD:R_0603_1608Metric

SOT-23-5   Package_TO_SOT_SMD:SOT-23-5   # trailing comment
)",
                  map));
    CHECK_EQ(map.entries.size(), std::size_t{2});
    CHECK_EQ(*map.entries.find("R-0603"), std::string("Resistor_SMD:R_0603_1608Metric"));
    CHECK_EQ(*map.entries.find("SOT-23-5"), std::string("Package_TO_SOT_SMD:SOT-23-5"));
}

TEST_CASE("a map entry naming no library is rejected") {
    FootprintMap map;
    std::size_t errors = 0;
    CHECK_FALSE(loadMap("R-0603  R_0603_1608Metric\n", map, &errors));
    CHECK_EQ(errors, std::size_t{1});
}

TEST_CASE("a map entry with nothing after it is rejected") {
    FootprintMap map;
    std::size_t errors = 0;
    CHECK_FALSE(loadMap("R-0603\n", map, &errors));
    CHECK_EQ(errors, std::size_t{1});
}

TEST_CASE("a footprint mapped twice is rejected") {
    FootprintMap map;
    std::size_t errors = 0;
    CHECK_FALSE(loadMap("R-0603  A:one\nR-0603  B:two\n", map, &errors));
    CHECK_EQ(errors, std::size_t{1});
    // The first wins, so the failure is reported rather than silently applied.
    CHECK_EQ(*map.entries.find("R-0603"), std::string("A:one"));
}

TEST_CASE("the resolution ladder takes each step in order") {
    FootprintMap map;
    CHECK(loadMap("R-0603  Resistor_SMD:R_0603_1608Metric\n", map));
    bool qualified = false;

    // 1. the map wins, even over a name that already has a library.
    CHECK_EQ(resolveFootprint(&map, "Fallback", "R-0603", qualified),
             std::string("Resistor_SMD:R_0603_1608Metric"));
    CHECK(qualified);

    // 2. an already-qualified name passes through.
    CHECK_EQ(resolveFootprint(&map, "Fallback", "Package_SO:SOIC-8", qualified),
             std::string("Package_SO:SOIC-8"));
    CHECK(qualified);

    // 3. the default library covers the rest.
    CHECK_EQ(resolveFootprint(&map, "Fallback", "C-0603", qualified),
             std::string("Fallback:C-0603"));
    CHECK(qualified);

    // 4. with no default, the raw name survives but is flagged.
    CHECK_EQ(resolveFootprint(&map, "", "C-0603", qualified), std::string("C-0603"));
    CHECK_FALSE(qualified);

    // No map at all is the same as an empty one.
    CHECK_EQ(resolveFootprint(nullptr, "Fallback", "C-0603", qualified),
             std::string("Fallback:C-0603"));
}

// ---------------------------------------------------------------------------
// The KiCad emitter
// ---------------------------------------------------------------------------

TEST_CASE("an unqualified footprint warns once and still exports") {
    Design design = tinyDesign();
    design.components.push_back(design.components[0]);  // same footprint again
    design.components.back().designator = "U2";

    std::size_t warnings = 0;
    ExportOptions options;
    std::string out = exportOf(design, options, &warnings);

    CHECK_EQ(warnings, std::size_t{1});  // one per distinct name, not per component
    CHECK(contains(out, "(footprint \"SOT-23-5\")"));
}

TEST_CASE("a mapped footprint is emitted qualified, with no warning") {
    FootprintMap map;
    CHECK(loadMap("SOT-23-5  Package_TO_SOT_SMD:SOT-23-5\n", map));

    ExportOptions options;
    options.footprints = &map;

    std::size_t warnings = 0;
    std::string out = exportOf(tinyDesign(), options, &warnings);

    CHECK_EQ(warnings, std::size_t{0});
    CHECK(contains(out, "(footprint \"Package_TO_SOT_SMD:SOT-23-5\")"));
}

TEST_CASE("a node carries its pin function and electrical type") {
    ExportOptions options;
    options.footprintLib = "Lib";
    std::string out = exportOf(tinyDesign(), options, nullptr);
    CHECK(contains(out, "(node (ref \"U1\") (pin \"5\") (pinfunction \"VOUT\") "
                        "(pintype \"power_out\"))"));
}

TEST_CASE("a supply pin's direction decides which way KiCad thinks it drives") {
    Design design = tinyDesign();
    ExportOptions options;
    options.footprintLib = "Lib";

    CHECK(contains(exportOf(design, options, nullptr), "(pintype \"power_out\")"));

    design.components[0].pins[0].direction = PortDir::In;
    CHECK(contains(exportOf(design, options, nullptr), "(pintype \"power_in\")"));

    design.components[0].pins[0].type = PinType::Signal;
    CHECK(contains(exportOf(design, options, nullptr), "(pintype \"input\")"));

    design.components[0].pins[0].type = PinType::OpenDrain;
    CHECK(contains(exportOf(design, options, nullptr), "(pintype \"open_collector\")"));

    design.components[0].pins[0].type = PinType::NC;
    CHECK(contains(exportOf(design, options, nullptr), "(pintype \"no_connect\")"));
}

TEST_CASE("a top-level component sits on the root sheet") {
    ExportOptions options;
    options.footprintLib = "Lib";
    std::string out = exportOf(tinyDesign(), options, nullptr);
    CHECK(contains(out, "(sheetpath (names \"/\") (tstamps \"/\"))"));
    CHECK(contains(out, "(tstamps \"" + pathUuid({"U1"}, 1) + "\")"));
}

TEST_CASE("a nested component carries its block hierarchy as a sheet path") {
    Design design = tinyDesign();
    design.components[0].path = {"power", "regulator", "U1"};

    ExportOptions options;
    options.footprintLib = "Lib";
    std::string out = exportOf(design, options, nullptr);

    CHECK(contains(out, "(sheetpath (names \"/power/regulator/\")"));
    CHECK(contains(out, pathUuid({"power", "regulator", "U1"}, 1)));
    CHECK(contains(out, pathUuid({"power", "regulator", "U1"}, 2)));
    // The reference is the flattened path, which is what a layout tool sees.
    CHECK(contains(out, "(comp (ref \"power_regulator_U1\")"));
}

TEST_CASE("export is byte-identical across runs") {
    // Spec 15.8. Cheap to assert here and the failure mode -- a hash or a map
    // leaking iteration order into the output -- is silent everywhere else.
    FootprintMap map;
    CHECK(loadMap("SOT-23-5  Package_TO_SOT_SMD:SOT-23-5\n", map));
    ExportOptions options;
    options.footprints = &map;

    Design design = tinyDesign();
    CHECK_EQ(exportOf(design, options, nullptr), exportOf(design, options, nullptr));
}

TEST_CASE("the other backends take the map but need no library") {
    FootprintMap map;
    CHECK(loadMap("SOT-23-5  Package_TO_SOT_SMD:SOT-23-5\n", map));
    ExportOptions options;
    options.footprints = &map;

    SourceManager sources;
    DiagEngine diags(sources);
    Design design = tinyDesign();

    for (ExportFormat format : {ExportFormat::Altium, ExportFormat::OrCad, ExportFormat::Allegro}) {
        std::string out = exportDesign(design, format, options, diags);
        CHECK(contains(out, "Package_TO_SOT_SMD:SOT-23-5"));
    }
    // A library nickname means nothing to these, so none of them warns.
    CHECK_EQ(diags.warningCount(), std::size_t{0});
}

TEST_MAIN()
