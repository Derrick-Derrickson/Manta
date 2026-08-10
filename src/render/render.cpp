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
        SheetLayout sheet = layoutPage(model, model.pages[i]);
        // A definition page is titled by its block; the top page by the design.
        sheet.tb.title = model.pages[i].definition ? model.pages[i].title : title;
        sheet.tb.sheet = std::format("Sheet {} of {}", i + 1, model.pages.size());
        sheets.push_back(std::move(sheet));
    }

    std::string out;
    emitDocument(out, model, sheets, title);
    return out;
}

}  // namespace manta::render
