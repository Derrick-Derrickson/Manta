// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
// The document shell: one self-contained HTML file, no external resources.
#pragma once

#include <string>
#include <vector>

#include "render/layout.h"

namespace manta::render {

// The page contract, which later agents extend rather than renegotiate:
//   <nav id="sidebar">        anchors to #page-<name>, one per page
//   <main id="sheets">        <section class="page" id="page-<name>"> per page,
//                             each holding one inline <svg class="sheet">
//   <aside id="info">         the details panel the script fills
//   <script type="application/json" id="cdata">
//                             designator -> {part, type, footprint, fitted,
//                             path, fields:[[k,v]...]}, in Design order
// Clicking any [data-net] toggles .hl on every element of that net, across
// pages; clicking a [data-c] symbol fills #info from the JSON blob.
void emitDocument(std::string& out, const RenderModel& model,
                  const std::vector<SheetLayout>& sheets, const std::string& title);

}  // namespace manta::render
