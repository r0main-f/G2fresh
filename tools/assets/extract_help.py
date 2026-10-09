#!/usr/bin/env python3
"""Extracts a short description of every module from the original editor's
help pages (Nord Modular G2 Help/nmg2_ref_-_*.htm): the first one or two
sentences of each module's page.

    extract_help.py <Nord Modular G2 Help folder>   -> data/module_help.json
"""
import html
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAX_CHARS = 260


def page_text(path):
    raw = open(path, "rb").read().decode("latin-1")
    title = re.search(r"(?is)<title>(.*?)</title>", raw)
    paras = [html.unescape(re.sub(r"(?s)<[^>]+>", " ", p)) for p in re.findall(r"(?is)<p[^>]*>(.*?)</p>", raw)]
    paras = [re.sub(r"\s+", " ", p).strip() for p in paras]
    paras = [p for p in paras if p and not p.startswith("You are here")]
    return (html.unescape(title.group(1)).strip() if title else ""), paras


def summary(paras, name):
    for p in paras:
        if p == name or len(p) < 25:
            continue
        sentences = [x for x in re.split(r"(?<=[.!?])\s+", p) if not x.startswith("See also")]
        if not sentences:
            continue
        out = sentences[0]
        for s in sentences[1:]:
            if len(out) + 1 + len(s) > MAX_CHARS:
                break
            out += " " + s
        return out if len(out) <= MAX_CHARS + 80 else out[:MAX_CHARS].rsplit(" ", 1)[0] + "..."
    return ""


def key(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(os.path.join(ROOT, "data", "modules.json")) as f:
        modules = json.load(f)["modules"]
    pages = {}
    for fn in os.listdir(argv[1]):
        if fn.startswith("nmg2_ref_-_") and fn.endswith(".htm"):
            title, paras = page_text(os.path.join(argv[1], fn))
            pages[key(fn[len("nmg2_ref_-_"):-4])] = (title, paras)
            pages.setdefault(key(title), (title, paras))
    out, missing = {}, []
    for m in modules:
        if m["kind"] != "module":
            continue
        for k in (key(m["shortName"]), key(m.get("fileName") or ""), key(m.get("longName") or "")):
            if k in pages:
                text = summary(pages[k][1], pages[k][0])
                if text:
                    out[m["shortName"]] = text
                    break
        else:
            missing.append(m["shortName"])
    with open(os.path.join(ROOT, "data", "module_help.json"), "w") as f:
        json.dump(dict(sorted(out.items())), f, indent=1, ensure_ascii=False)
    print(f"{len(out)} descriptions; no help page for: {', '.join(missing) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
