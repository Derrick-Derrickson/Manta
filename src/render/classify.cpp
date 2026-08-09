// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/classify.h"

namespace manta::render {

SymbolKind classifySymbol(const Component& c) {
    switch (c.partType) {
        case PartType::BoardConnector:
        case PartType::CableConnector: return SymbolKind::Connector;
        default: return SymbolKind::Generic;
    }
}

}  // namespace manta::render
