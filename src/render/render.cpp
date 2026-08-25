// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/render.h"

#include <format>
#include <vector>

#include "render/html.h"
#include "render/layout.h"
#include "render/model.h"

namespace manta::render {

std::string renderSchematic(const Design& design, const RenderOptions& options) {
    RenderModel model = buildRenderModel(design);

    std::string title = options.title;
    if (title.empty()) title = design.top.empty() ? "schematic" : design.top;

    std::vector<SheetLayout> sheets;
    sheets.reserve(model.pages.size());
    for (std::size_t i = 0; i < model.pages.size(); ++i) {
        const RenderPage& page = model.pages[i];
        SheetLayout sheet = layoutPage(model, page);
        // A definition page is titled by its block; the top page by the design.
        sheet.tb.title = page.definition ? page.title : title;
        sheet.tb.sheet = std::format("Sheet {} of {}", i + 1, model.pages.size());

        // '&RENDER=WIRE' (spec 11.3, revision 1.6) is a command with a
        // diagnosable failure: a wired net shows no name, so any mark still
        // standing for it on the finished sheet is the fallback the author
        // asked to be told about.
        if (options.warnings) {
            std::vector<std::uint8_t> named(page.nets.size(), 0);
            for (const MarkItem& mk : sheet.marks) {
                if (mk.net >= 0) named[static_cast<std::size_t>(mk.net)] = 1;
            }
            for (const RailBarItem& bar : sheet.bars) {
                if (bar.net >= 0) named[static_cast<std::size_t>(bar.net)] = 1;
            }
            for (std::size_t ni = 0; ni < page.nets.size(); ++ni) {
                if (page.nets[ni].force != ForceMode::Wire || !named[ni]) continue;
                const RenderNet& rn = page.nets[ni];
                std::string why = rn.crossing ? "its pins sit in more than one room"
                                : rn.direction != PortDir::None
                                    ? "it reaches a block port, which connects by name"
                                    : "no route joined every pin";
                options.warnings->push_back(
                    std::format("'&RENDER=WIRE' net '{}' is connected by name on {}: {}",
                                rn.display, page.id, why));
            }
        }
        sheets.push_back(std::move(sheet));
    }

    std::string out;
    emitDocument(out, model, sheets, title);
    return out;
}

}  // namespace manta::render
