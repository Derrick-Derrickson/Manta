// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// One sheet as inline SVG.
//
// The contract with html.cpp's script: every wire, stub, label, ground and
// rail element carries data-net="<flat design-wide name>" -- the same name on
// every page, which is what makes a net highlight cross pages -- while the
// drawn text is the page-local spelling. Every symbol group carries
// data-c="<designator>". Coordinates are integers throughout.
#pragma once

#include <string>

#include "render/layout.h"

namespace manta::render {

void xmlEscape(std::string& out, std::string_view text);

void emitSheetSvg(std::string& out, const RenderModel& model, const RenderPage& page,
                  const SheetLayout& sheet);

}  // namespace manta::render
