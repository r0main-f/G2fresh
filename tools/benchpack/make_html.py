#!/usr/bin/env python3
"""Makes the printable pages of the hardware bench pack from its instructions.

    python3 tools/benchpack/make_html.py [--check]

Writes bench/hardware-pack/README.html and README.fr.html from README.md and README.fr.md: one self-contained file
each, laid out to print on two A4 or Letter pages. --check exits 1 if the committed pages are not what the
instructions give (after editing a README, run this again).

Plain Python. It reads the small part of Markdown the instructions use: headings, paragraphs, numbered and bulleted
lists (one level, with indented follow-on lines), tables, **bold**, *italic*, `code` and [links](url).
"""
import html
import os
import re
import sys

PACK = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "bench", "hardware-pack")
PAGES = {"README.md": ("README.html", "en"), "README.fr.md": ("README.fr.html", "fr")}

CSS = """
@page { size: auto; margin: 11mm 12mm; }
* { box-sizing: border-box; }
html { font: 9.6pt/1.32 -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; color: #111; background: #fff; }
body { max-width: 190mm; margin: 0 auto; padding: 6mm 4mm; }
h1 { font-size: 15pt; margin: 0 0 1.5mm; }
h2 { font-size: 11pt; margin: 3.2mm 0 1mm; padding-top: 1mm; border-top: 0.3mm solid #999; break-after: avoid; }
p { margin: 0 0 1.4mm; }
ol, ul { margin: 0 0 1.4mm; padding-left: 5.5mm; }
li { margin: 0 0 0.5mm; break-inside: avoid; }
table { border-collapse: collapse; width: 100%; margin: 1mm 0 1.6mm; font-size: 8.8pt; break-inside: avoid; }
th, td { border: 0.2mm solid #999; padding: 0.4mm 1.5mm; text-align: left; vertical-align: top; }
th { background: #eee; }
code { font: 8.8pt ui-monospace, Menlo, Consolas, monospace; background: #f1f1f1; padding: 0 0.6mm; border-radius: 0.6mm; }
a { color: inherit; }
@media screen and (max-width: 700px) { html { font-size: 12pt; } table { font-size: 10pt; } }
@media print { body { padding: 0; max-width: none; } th { -webkit-print-color-adjust: exact; print-color-adjust: exact; } }
"""


def inline(text):
    text = html.escape(text, quote=False)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![*\w])\*([^*]+)\*(?![*\w])", r"<em>\1</em>", text)
    # a link to the other language's instructions points at its printable page
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", lambda m: '<a href="%s">%s</a>' % (re.sub(r"\.md$", ".html", m.group(2)), m.group(1)), text)
    return text


def convert(markdown):
    lines = markdown.split("\n")
    out, i, title = [], 0, ""
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        m = re.match(r"(#{1,3}) (.*)", line)
        if m:
            level = len(m.group(1))
            if level == 1 and not title:
                title = re.sub(r"[*`]", "", m.group(2))
            out.append("<h%d>%s</h%d>" % (level, inline(m.group(2)), level))
            i += 1
            continue
        if line.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            out.append("<table>")
            head = len(rows) > 1 and all(re.fullmatch(r":?-+:?", c) for c in rows[1])
            for n, row in enumerate(rows):
                if head and n == 1:
                    continue
                cell = "th" if head and n == 0 else "td"
                out.append("<tr>" + "".join("<%s>%s</%s>" % (cell, inline(c), cell) for c in row) + "</tr>")
            out.append("</table>")
            continue
        m = re.match(r"(\d+\.|-) (.*)", line)
        if m:
            tag = "ul" if m.group(1) == "-" else "ol"
            pattern = r"- (.*)" if tag == "ul" else r"\d+\. (.*)"
            out.append("<%s>" % tag)
            while i < len(lines) and re.match(pattern, lines[i]):
                item = re.match(pattern, lines[i]).group(1)
                i += 1
                while i < len(lines) and lines[i].startswith("  ") and lines[i].strip():  # a follow-on line
                    item += " " + lines[i].strip()
                    i += 1
                out.append("<li>%s</li>" % inline(item))
            out.append("</%s>" % tag)
            continue
        para = []
        while i < len(lines) and lines[i].strip() and not re.match(r"(#{1,3} |\||\d+\. |- )", lines[i]):
            para.append(lines[i].strip())
            i += 1
        out.append("<p>%s</p>" % inline(" ".join(para)))
    return title, "\n".join(out)


def page(markdown, lang):
    title, body = convert(markdown)
    return ("<!doctype html>\n<html lang=\"%s\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "<title>%s</title>\n<style>%s</style>\n</head>\n<body>\n%s\n</body>\n</html>\n" % (lang, html.escape(title), CSS, body))


def main():
    check = "--check" in sys.argv[1:]
    stale = 0
    for source, (target, lang) in PAGES.items():
        with open(os.path.join(PACK, source), encoding="utf-8") as f:
            text = page(f.read(), lang)
        path = os.path.join(PACK, target)
        if check:
            current = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
            if current != text:
                print("%s is not what %s gives: run tools/benchpack/make_html.py" % (target, source))
                stale += 1
        else:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            print("wrote %s" % path)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
