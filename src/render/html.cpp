// SPDX-FileCopyrightText: 2026 Tom
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/html.h"

#include <format>

#include "render/svg.h"

namespace manta::render {

namespace {

// The whole palette lives here, as custom properties; the SVG carries classes
// only. Screen chrome around the sheet is dark so the cream paper reads like
// paper; print strips the chrome and puts one sheet per A3 page.
constexpr std::string_view kStyle = R"__(
:root {
  --sheet: #FFFCF0;        /* cream paper */
  --frame: #57534a;        /* sheet border and zone text */
  --body-fill: #FFF6A0;    /* component body */
  --body-stroke: #800000;  /* component outline, designators */
  --wire: #0000A0;         /* nets, stubs, pin lines */
  --pinnum: #6a6a6a;
  --pinname: #1c1c1c;
  --label: #A00000;        /* net labels */
  --nc: #9a9a9a;
  --hl: #FF7F00;           /* highlight */
  --sheet-fill: #C6ECB8;   /* sheet-symbol body */
  --sheet-stroke: #1F6B2A; /* sheet-symbol border, instance designator */
}
* { box-sizing: border-box; }
body { margin: 0; display: flex; height: 100vh; background: #3c3c3a;
       font-family: Georgia, 'Times New Roman', serif; }
#sidebar { flex: none; width: 200px; overflow: auto; background: #2c2c2a;
           color: #ddd; padding: 14px; }
#sidebar h1 { font-size: 15px; margin: 0 0 12px; color: #fff; font-weight: normal;
              letter-spacing: 1px; text-transform: uppercase; }
#sidebar a { display: block; color: #9db4e8; text-decoration: none; font-size: 13px;
             padding: 4px 6px; border-radius: 3px; }
#sidebar a.cur, #sidebar a:hover { background: #45453f; color: #fff; }
#sheets { flex: 1; overflow: auto; padding: 24px; }
section.page { margin: 0 auto 24px; max-width: 1500px; }
svg.sheet { display: block; width: 100%; height: auto; background: var(--sheet);
            box-shadow: 0 2px 14px rgba(0,0,0,.55); }
#info { flex: none; width: 270px; overflow: auto; background: #f6f3ea;
        border-left: 1px solid #c9c4b4; padding: 14px 16px; font-size: 13px; }
#info h2 { font-size: 12px; letter-spacing: 1px; text-transform: uppercase;
           color: #6a6455; margin: 0 0 10px; font-weight: normal; }
#info h3 { font-size: 17px; margin: 0 0 8px; color: #800000;
           font-family: 'Courier New', monospace; }
#info h4 { font-size: 11px; letter-spacing: 1px; text-transform: uppercase;
           color: #6a6455; margin: 14px 0 4px; font-weight: normal; }
#info dl { margin: 0; }
#info dt { font-weight: bold; font-size: 11px; color: #444; }
#info dd { margin: 0 0 6px; font-family: 'Courier New', monospace; font-size: 12px;
           overflow-wrap: anywhere; }
#info .hint { color: #8a8474; font-style: italic; }

/* sheet artwork */
.sheet-bg { fill: var(--sheet); }
.frame { fill: none; stroke: var(--frame); stroke-width: 1; }
.zone { font-size: 9px; fill: var(--frame); font-family: Georgia, serif; }
.tb { fill: none; stroke: var(--frame); stroke-width: 1; }
.tbkey { font-size: 8px; fill: var(--frame); font-family: Georgia, serif;
         letter-spacing: 1px; }
.tbval { font-size: 10px; fill: #1c1c1c; font-family: 'Courier New', monospace; }
.sym .body { fill: var(--body-fill); stroke: var(--body-stroke); stroke-width: 1; }
/* classic symbol artwork: blue strokes on the bare paper, like the wires */
.sym .glyph { fill: none; stroke: var(--wire); stroke-width: 1;
              stroke-linecap: round; stroke-linejoin: round; }
.sym .glyph.fill { fill: var(--wire); }
.sym .mark { font-size: 10px; font-weight: bold; fill: var(--wire);
             font-family: 'Courier New', monospace; }
.refdes { font-size: 11px; font-weight: bold; fill: var(--body-stroke);
          font-family: 'Courier New', monospace; }
.partname { font-size: 9px; fill: #1c1c1c; font-family: 'Courier New', monospace; }
.pinname { font-size: 10px; fill: var(--pinname); font-family: 'Courier New', monospace; }
.pinnum { font-size: 7px; fill: var(--pinnum); font-family: 'Courier New', monospace; }
/* a planned pin's net name, drawn inside beside the silicon name */
.pinnet { font-size: 8px; fill: var(--label); font-family: 'Courier New', monospace; }
line.wire, polyline.wire { stroke: var(--wire); stroke-width: 1; fill: none; }
line.railbar { stroke: var(--wire); stroke-width: 3; }
circle.dot { fill: var(--wire); stroke: none; }
.netlabel { font-size: 9px; font-weight: bold; fill: var(--label);
            font-family: 'Courier New', monospace; }
.portflag .flag { fill: var(--body-fill); stroke: var(--body-stroke); stroke-width: 1; }
.portflag .flagtext { font-size: 9px; font-weight: bold; fill: var(--label);
                      font-family: 'Courier New', monospace; }
rect.room { fill: none; stroke: var(--wire); stroke-width: 1; }
.roomtitle { font-size: 12px; fill: var(--wire); letter-spacing: 2px;
             font-family: Georgia, serif; }
text.nc { fill: var(--nc); }
/* the no-connect cross, in the grey that already de-emphasises a NC pin's
   number and name, so the whole pin reads as one deliberately dead thing */
line.noconn { stroke: var(--nc); stroke-width: 1; }
.sheetnote { font-size: 10px; font-style: italic; fill: var(--frame);
             font-family: Georgia, serif; }

/* sheet symbols: child block instances, linking to the definition's page */
.sheetsym { cursor: pointer; }
.sheetsym .sbody { fill: var(--sheet-fill); stroke: var(--sheet-stroke); stroke-width: 1; }
.sheetsym .sheetref { font-size: 11px; font-weight: bold; fill: var(--sheet-stroke);
                      font-family: 'Courier New', monospace; }
.sheetsym .sheetname { font-size: 10px; font-weight: bold; fill: #1c1c1c;
                       font-family: 'Courier New', monospace; }
.sheetsym .sheettag { font-size: 7px; font-style: italic; fill: var(--sheet-stroke);
                      font-family: Georgia, serif; }
.sheetsym .sheetrule { stroke: var(--sheet-stroke); stroke-width: 1; }
.sheetsym .ptab { fill: var(--body-fill); stroke: var(--body-stroke); stroke-width: 1; }
.sheetsym g.hl .ptab { stroke: var(--hl); stroke-width: 2; }

/* interactivity */
[data-net], .sym { cursor: pointer; }
line.hl, .hl line, polyline.hl, .hl polyline, circle.hl { stroke: var(--hl); stroke-width: 2; }
circle.dot.hl { fill: var(--hl); }
text.hl, .hl text { fill: var(--hl); }
.portflag.hl .flag { stroke: var(--hl); stroke-width: 2; }
.sym.sel .body, .sym.sel .glyph { stroke: var(--hl); stroke-width: 2; }
.sym.sel .glyph.fill { fill: var(--hl); }

/* print: one sheet per A3 landscape page, chrome stripped */
@page { size: A3 landscape; margin: 0; }
@media print {
  body { display: block; height: auto; background: #fff; }
  #sidebar, #info { display: none; }
  #sheets { padding: 0; overflow: visible; }
  section.page { max-width: none; margin: 0; break-after: page; page-break-after: always; }
  svg.sheet { box-shadow: none; width: 100%; height: 100vh; }
}
)__";

constexpr std::string_view kScript = R"__(
(function () {
  'use strict';
  var data = JSON.parse(document.getElementById('cdata').textContent);
  var info = document.getElementById('info-body');
  var activeNet = null, activeComp = null;

  function row(dl, k, v) {
    var dt = document.createElement('dt'); dt.textContent = k;
    var dd = document.createElement('dd'); dd.textContent = v;
    dl.appendChild(dt); dl.appendChild(dd);
  }
  function hint() {
    info.textContent = '';
    var p = document.createElement('p');
    p.className = 'hint';
    p.textContent = 'Click a component for its details, or any wire or label to trace the net.';
    info.appendChild(p);
  }
  function applyNet() {
    var els = document.querySelectorAll('[data-net]');
    for (var i = 0; i < els.length; i++) {
      var on = activeNet !== null && els[i].getAttribute('data-net') === activeNet;
      els[i].classList[on ? 'add' : 'remove']('hl');
    }
  }
  function applyComp() {
    var els = document.querySelectorAll('[data-c]');
    for (var i = 0; i < els.length; i++) {
      els[i].classList[els[i].getAttribute('data-c') === activeComp ? 'add' : 'remove']('sel');
    }
  }
  function showComp(des) {
    var c = data[des];
    if (!c) return;
    info.textContent = '';
    var h = document.createElement('h3'); h.textContent = des; info.appendChild(h);
    var dl = document.createElement('dl');
    row(dl, 'Part', c.part);
    row(dl, 'Type', c.type);
    if (c.footprint) row(dl, 'Footprint', c.footprint);
    row(dl, 'Fitted', c.fitted ? 'yes' : 'no');
    if (c.path) row(dl, 'Path', c.path);
    info.appendChild(dl);
    if (c.fields.length) {
      var h4 = document.createElement('h4'); h4.textContent = 'Fields'; info.appendChild(h4);
      var fl = document.createElement('dl');
      for (var i = 0; i < c.fields.length; i++) row(fl, c.fields[i][0], c.fields[i][1]);
      info.appendChild(fl);
    }
  }
  function showNet(name) {
    info.textContent = '';
    var h = document.createElement('h3'); h.textContent = name; info.appendChild(h);
    var els = document.querySelectorAll('[data-net]'), n = 0;
    for (var i = 0; i < els.length; i++) {
      if (els[i].getAttribute('data-net') === name) n++;
    }
    var p = document.createElement('p');
    p.textContent = 'Net — ' + n + ' element(s) highlighted.';
    info.appendChild(p);
  }

  document.addEventListener('click', function (e) {
    var el = e.target instanceof Element ? e.target : null;
    if (!el) return;
    var net = el.closest('[data-net]');
    if (net) {
      var name = net.getAttribute('data-net');
      activeNet = activeNet === name ? null : name;
      applyNet();
      if (activeNet === null) hint(); else showNet(activeNet);
      return;
    }
    var sym = el.closest('[data-c]');
    if (sym) {
      var des = sym.getAttribute('data-c');
      activeComp = activeComp === des ? null : des;
      applyComp();
      if (activeComp === null) hint(); else showComp(des);
    }
  });

  function onHash() {
    var links = document.querySelectorAll('#sidebar a');
    for (var i = 0; i < links.length; i++) {
      var cur = links[i].getAttribute('href') === location.hash;
      links[i].classList[cur ? 'add' : 'remove']('cur');
    }
  }
  window.addEventListener('hashchange', onHash);
  onHash();
  hint();
})();
)__";

std::string esc(std::string_view text) {
    std::string out;
    xmlEscape(out, text);
    return out;
}

void jsonStr(std::string& out, std::string_view s) {
    out += '"';
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            // '<' escaped so no field value can ever form "</script>".
            case '<': out += "\\u003c"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else out += ch;
                break;
        }
    }
    out += '"';
}

// designator -> details, in Design order, so the blob is deterministic.
void emitComponentData(std::string& out, const Design& design) {
    out += "<script type=\"application/json\" id=\"cdata\">{";
    bool firstC = true;
    for (const Component& c : design.components) {
        if (!firstC) out += ',';
        firstC = false;
        jsonStr(out, c.designator);
        out += ":{\"part\":";
        jsonStr(out, c.partName);
        out += ",\"type\":";
        jsonStr(out, c.type);
        out += ",\"footprint\":";
        jsonStr(out, c.footprint);
        out += std::format(",\"fitted\":{}", c.fitted ? "true" : "false");
        out += ",\"path\":";
        std::string path;
        for (const std::string& p : c.path) {
            if (!path.empty()) path += '/';
            path += p;
        }
        jsonStr(out, path);
        out += ",\"fields\":[";
        bool firstF = true;
        for (const auto& [name, value] : c.fields) {
            if (!firstF) out += ',';
            firstF = false;
            out += '[';
            jsonStr(out, name);
            out += ',';
            jsonStr(out, value);
            out += ']';
        }
        out += "]}";
    }
    out += "}</script>\n";
}

}  // namespace

void emitDocument(std::string& out, const RenderModel& model,
                  const std::vector<SheetLayout>& sheets, const std::string& title) {
    out += "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>";
    xmlEscape(out, title);
    out += " &#8212; manta render</title>\n<style>";
    out += kStyle;
    out += "</style>\n</head>\n<body>\n";

    out += "<nav id=\"sidebar\">\n<h1>";
    xmlEscape(out, title);
    out += "</h1>\n<ul style=\"list-style:none;margin:0;padding:0\">\n";
    for (std::size_t i = 0; i < model.pages.size(); ++i) {
        const RenderPage& page = model.pages[i];
        out += std::format("<li><a href=\"#{}\">{} &#183; ", esc(page.id), i + 1);
        xmlEscape(out, page.title);
        out += "</a></li>\n";
    }
    out += "</ul>\n</nav>\n";

    out += "<main id=\"sheets\">\n";
    for (std::size_t i = 0; i < model.pages.size() && i < sheets.size(); ++i) {
        out += std::format("<section class=\"page\" id=\"{}\">\n", esc(model.pages[i].id));
        emitSheetSvg(out, model, model.pages[i], sheets[i]);
        out += "</section>\n";
    }
    out += "</main>\n";

    out += "<aside id=\"info\">\n<h2>Details</h2>\n<div id=\"info-body\"></div>\n</aside>\n";

    emitComponentData(out, *model.design);
    out += "<script>";
    out += kScript;
    out += "</script>\n</body>\n</html>\n";
}

}  // namespace manta::render
