#!/usr/bin/env python3
"""Turns docs/help.md into docs/help.html (what the Help command opens).
Needs the python3-markdown package. Run it after editing help.md and commit both files."""
import html, os, re, sys
import markdown

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
src = open(os.path.join(root, "docs", "help.md"), encoding="utf-8").read()
title = re.match(r"#\s+(.*)", src).group(1)
body = markdown.markdown(src, extensions=["tables"])

page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ font: 16px/1.55 system-ui, sans-serif; max-width: 46rem; margin: 2rem auto; padding: 0 1rem; }}
  h1, h2 {{ line-height: 1.2; }}
  h2 {{ margin-top: 2rem; border-bottom: 1px solid #8884; padding-bottom: .2rem; }}
  table {{ border-collapse: collapse; width: 100%; }}
  th, td {{ border: 1px solid #8886; padding: .35rem .6rem; text-align: left; vertical-align: top; }}
  th {{ background: #8882; }}
  code, kbd {{ font-family: ui-monospace, monospace; background: #8882; padding: 0 .25em; border-radius: 3px; }}
</style>
</head>
<body>
{body}
</body>
</html>
"""
open(os.path.join(root, "docs", "help.html"), "w", encoding="utf-8").write(page)
