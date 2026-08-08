#!/usr/bin/env python3
"""Renders a Markdown document to PDF for the release package.

Markdown to HTML with python-markdown, then HTML to PDF with headless Chromium.
No LaTeX: the documents are prose, tables and code, and a browser sets all three
well without a TeX installation to carry around.

    tools/make-pdf.py docs/spec.md out/spec.pdf "The Manta Language"
"""

import html
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import markdown

CSS = """
@page { size: A4; margin: 20mm 18mm; }

body {
    font-family: "Charter", "Georgia", "Times New Roman", serif;
    font-size: 10.5pt;
    line-height: 1.5;
    color: #1a1a1a;
    max-width: none;
}

/* A heading should not be the last thing on a page. */
h1, h2, h3, h4 { font-family: "Helvetica Neue", Helvetica, Arial, sans-serif;
                 line-height: 1.25; break-after: avoid; page-break-after: avoid; }
h1 { font-size: 20pt; margin: 0 0 0.6em; padding-bottom: 0.25em;
     border-bottom: 2px solid #1a1a1a; }
h2 { font-size: 15pt; margin: 1.6em 0 0.5em; padding-bottom: 0.15em;
     border-bottom: 1px solid #c8c8c8; break-before: auto; }
h3 { font-size: 12pt; margin: 1.3em 0 0.4em; }
h4 { font-size: 10.5pt; margin: 1.1em 0 0.3em; font-weight: 600; }

p, ul, ol { margin: 0 0 0.7em; }
li { margin-bottom: 0.2em; }

code, pre {
    font-family: "DejaVu Sans Mono", "Menlo", "Consolas", monospace;
    font-size: 9pt;
}
code { background: #f2f2f2; padding: 0.1em 0.3em; border-radius: 2px; }
pre {
    background: #f7f7f7;
    border: 1px solid #e0e0e0;
    border-left: 3px solid #999;
    border-radius: 2px;
    padding: 0.6em 0.8em;
    overflow-x: auto;
    white-space: pre-wrap;
    word-wrap: break-word;
    break-inside: avoid;
}
pre code { background: none; padding: 0; font-size: 8.6pt; }

table {
    border-collapse: collapse;
    margin: 0.7em 0 1em;
    font-size: 9.2pt;
    width: 100%;
    break-inside: avoid;
}
th, td { border: 1px solid #ccc; padding: 0.32em 0.55em; text-align: left;
         vertical-align: top; }
th { background: #efefef; font-weight: 600; }
td code, th code { font-size: 8.6pt; }

blockquote {
    margin: 1em 0;
    padding: 0.6em 1em;
    background: #f4f6f8;
    border-left: 3px solid #7a8896;
    break-inside: avoid;
}
blockquote p:last-child { margin-bottom: 0; }

hr { border: none; border-top: 1px solid #d5d5d5; margin: 1.6em 0; }
a { color: #1a1a1a; text-decoration: none; border-bottom: 1px solid #bbb; }

.title-block { margin-bottom: 2em; }
.title-block .subtitle { color: #666; font-size: 10pt; margin-top: -0.4em; }
"""


def find_chromium() -> str:
    for name in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("no chromium or chrome on PATH; cannot render PDF")


def render(source: Path, output: Path, title: str) -> None:
    text = source.read_text(encoding="utf-8")

    body = markdown.markdown(
        text,
        extensions=["tables", "fenced_code", "sane_lists", "attr_list"],
        output_format="html5",
    )

    page = (
        "<!doctype html><html><head><meta charset='utf-8'>"
        f"<title>{html.escape(title)}</title>"
        f"<style>{CSS}</style></head><body>{body}</body></html>"
    )

    output.parent.mkdir(parents=True, exist_ok=True)

    # Chromium is commonly packaged as a confined snap, which cannot reach /tmp
    # at all and cannot write to a dot-directory at the top of $HOME. Staging
    # beside the output avoids guessing which paths the confinement allows: if
    # the caller can write there, so can the browser.
    staging = Path(os.environ.get("MANTA_PDF_STAGING", output.parent))
    staging.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(dir=staging) as tmp:
        page_path = Path(tmp) / "page.html"
        page_path.write_text(page, encoding="utf-8")
        staged_pdf = Path(tmp) / "out.pdf"

        result = subprocess.run(
            [
                find_chromium(),
                "--headless",
                "--disable-gpu",
                "--no-sandbox",
                "--no-pdf-header-footer",
                "--run-all-compositor-stages-before-draw",
                "--virtual-time-budget=10000",
                f"--print-to-pdf={staged_pdf}",
                page_path.as_uri(),
            ],
            capture_output=True,
            text=True,
        )

        if not staged_pdf.exists() or staged_pdf.stat().st_size < 1000:
            sys.exit(f"chromium produced no usable PDF for {source}\n{result.stderr[:2000]}")
        shutil.move(str(staged_pdf), str(output))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    render(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3])
    print(f"  {sys.argv[2]}  ({Path(sys.argv[2]).stat().st_size // 1024} KB)")
