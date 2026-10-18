#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Build the design review: pictures of every token page, component and panel, plus the audits.

    python3 src/design/review-design.py [--binary build/bin/kdenlive] [--output build/design-review]
                                        [--project file.kdenlive] [--scale 1] [--strict] [--skip-run]

Runs the built editor headless with `--design-review`, which writes the review pages, the main
window and every panel as PNG files in both themes, and review.json with the live audit (tokens,
contrast, type styles, icons, and the fonts, margins, sizes, style sheets and texts that bypass
the tokens). Adds the source audits (check-design.py, icon names outside the theme, .ui files
with values off the scale, literal colors in C++) and writes index.html: one page to read with
a browser, or to hand to an agent, when working on the design guidelines.

With --strict the exit code is 1 when a source audit or a contrast check fails.
"""

import argparse
import html
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent
ROOT = SRC.parent
SCALE = {0, 2, 4, 8, 16, 32, 64, 128}
UI_LENGTHS = {"margin", "leftMargin", "topMargin", "rightMargin", "bottomMargin", "spacing", "horizontalSpacing", "verticalSpacing"}
ICON_PATTERNS = [
    re.compile(r'fromTheme\(QStringLiteral\("([^"]+)"\)'),
    re.compile(r'fromTheme\("([^"]+)"'),
    re.compile(r'<iconset theme="([^"]+)"'),
    re.compile(r'icon\.name:\s*"([^"]+)"'),
    re.compile(r'\.source:\s*"([^"]+)"'),
]
COLOR_PATTERNS = [
    re.compile(r"QColor\(\s*\d+\s*,\s*\d+\s*,\s*\d+"),
    re.compile(r'QColor\((QStringLiteral\()?"#'),
    re.compile(r"\bQt::(red|green|blue|yellow|white|black|cyan|magenta|gray|darkGray|lightGray)\b"),
]
COLOR_SKIP = ("scopes/", "titler/", "utils/colortools", "utils/qcolorutils", "utils/designtokens", "utils/designaudit", "widgets/colorpickerwidget", "widgets/choosecolorwidget")


def load_check_design():
    spec = importlib.util.spec_from_file_location("check_design", HERE / "check-design.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def theme_icons():
    return sorted(path.stem for path in (HERE / "icons" / "KdenlivePro-dark" / "scalable").glob("*.svg"))


def source_files(*suffixes):
    for path in sorted(SRC.rglob("*")):
        if path.suffix in suffixes and "design/icons" not in str(path):
            yield path


def icon_audit():
    """Icon names the sources ask for that the theme does not draw: {name: {count, files}}"""
    known = set(theme_icons())
    usage = defaultdict(lambda: {"count": 0, "files": set()})
    for path in source_files(".cpp", ".h", ".hpp", ".ui", ".qml"):
        text = path.read_text(errors="replace")
        for pattern in ICON_PATTERNS:
            for match in pattern.finditer(text):
                name = match.group(1)
                if "/" in name or "." in name or name in known or not name:
                    continue
                entry = usage[name]
                entry["count"] += 1
                entry["files"].add(str(path.relative_to(ROOT)))
    return [{"name": name, "count": entry["count"], "files": sorted(entry["files"])[:8]} for name, entry in sorted(usage.items(), key=lambda item: -item[1]["count"])]


def ui_audit():
    """Values in .ui files that bypass the tokens: lengths off the scale, point sizes, style sheets, literal colors"""
    findings = []
    for path in source_files(".ui"):
        try:
            tree = ElementTree.parse(path)
        except ElementTree.ParseError as error:
            findings.append({"file": str(path.relative_to(ROOT)), "kind": "parse", "value": str(error)})
            continue
        relative = str(path.relative_to(ROOT))
        for prop in tree.iter("property"):
            name = prop.get("name", "")
            number = prop.find("number")
            if name in UI_LENGTHS and number is not None and number.text and int(number.text) not in SCALE:
                findings.append({"file": relative, "kind": "length", "property": name, "value": int(number.text)})
            elif name == "font":
                size = prop.find("font/pointsize")
                if size is not None and size.text:
                    findings.append({"file": relative, "kind": "pointsize", "property": name, "value": int(size.text)})
            elif name == "styleSheet":
                string = prop.find("string")
                if string is not None and string.text and string.text.strip():
                    findings.append({"file": relative, "kind": "styleSheet", "property": name, "value": string.text.strip()[:120]})
            elif name in ("color", "textColor", "backgroundColor") and prop.find("color") is not None:
                findings.append({"file": relative, "kind": "color", "property": name, "value": ElementTree.tostring(prop.find("color"), encoding="unicode").strip()[:80]})
    return findings


def color_audit():
    """Literal colors in C++ outside content code, unless marked design:allow"""
    findings = []
    for path in source_files(".cpp"):
        relative = str(path.relative_to(SRC))
        if any(relative.startswith(skip) or skip in relative for skip in COLOR_SKIP):
            continue
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            stripped = line.strip()
            if "design:allow" in line or stripped.startswith("//") or stripped.startswith("*"):
                continue
            if any(pattern.search(line) for pattern in COLOR_PATTERNS):
                findings.append({"file": f"src/{relative}", "line": number, "value": stripped[:140]})
    return findings


def make_fixture(folder):
    """A small project from the MCP test fixture so the window shows a timeline, bin and viewer"""
    ffmpeg = shutil.which("ffmpeg")
    fixture = ROOT / "tests" / "mcp" / "fixture.kdenlive"
    if not ffmpeg or not fixture.exists():
        return None
    media = folder / "source.mkv"
    subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "color=c=0x3a6ea5:s=640x360:r=30", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                    "-t", "20", "-c:v", "libx264", "-preset", "ultrafast", "-threads", "1", "-c:a", "pcm_s16le", str(media)], check=True)
    project = folder / "review.kdenlive"
    project.write_text(fixture.read_text().replace("@MEDIA@", str(media).replace("&", "&amp;")))
    return project


def run_editor(binary, output, project, scale, window, settle):
    runtime = Path(tempfile.mkdtemp(prefix="kdenlive-design-review-"))
    for name in ("config", "data", "cache", "state", "tmp"):
        (runtime / name).mkdir()
    melt = shutil.which("melt-7") or shutil.which("melt") or ""
    (runtime / "config" / "kdenliverc").write_text(
        "[startup]\nlastSeenVersionMajor=26\nlastSeenVersionMinor=8\n"
        f"[env]\nmeltpath={melt}\nffmpegpath={shutil.which('ffmpeg') or ''}\nffprobepath={shutil.which('ffprobe') or ''}\n"
        "[unmanaged]\ndefault_profile=atsc_1080p_25\n[sdl]\naudiobackend=null\n")
    screens = runtime / "offscreen.json"
    screens.write_text(json.dumps({"screens": [{"name": "review", "x": 0, "y": 0, "width": window[0] + 200, "height": window[1] + 200, "logicalDpi": 96, "dpr": 1}]}))
    env = dict(os.environ)
    env.update({
        "QT_QPA_PLATFORM": f"offscreen:configfile={screens}", "QT_QPA_PLATFORMTHEME": "", "QT_STYLE_OVERRIDE": "Fusion", "QT_QUICK_BACKEND": "software",
        "QT_LOGGING_RULES": "*.debug=false", "MLT_NO_VDPAU": "1", "KDENLIVE_MCP_BRIDGE": "0",
        "XDG_CONFIG_HOME": str(runtime / "config"), "XDG_DATA_HOME": str(runtime / "data"), "XDG_CACHE_HOME": str(runtime / "cache"),
        "XDG_STATE_HOME": str(runtime / "state"), "TMPDIR": str(runtime / "tmp"),
        "XDG_DATA_DIRS": f"{Path(binary).resolve().parent.parent / 'share'}:/usr/local/share:/usr/share",
        "KDENLIVE_DESIGN_REVIEW_WINDOW": f"{window[0]}x{window[1]}", "KDENLIVE_DESIGN_REVIEW_SETTLE_MS": str(settle),
    })
    for variable in ("WAYLAND_DISPLAY", "DISPLAY"):
        env.pop(variable, None)
    command = [str(binary), "--design-review", str(output), "--no-welcome"]
    if project:
        command.append(str(project))
    print("Running", " ".join(command))
    with open(output / "run.log", "w") as log:
        try:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        except subprocess.TimeoutExpired:
            print("The editor did not exit within 300 s; see", output / "run.log", file=sys.stderr)
            return False
    if result.returncode != 0:
        print(f"The editor exited with {result.returncode}; see", output / "run.log", file=sys.stderr)
    shutil.rmtree(runtime, ignore_errors=True)
    return (output / "review.json").exists()


def escape(value):
    return html.escape(str(value))


def table(headers, rows):
    if not rows:
        return "<p class='empty'>Nothing found.</p>"
    head = "".join(f"<th>{escape(h)}</th>" for h in headers)
    body = "".join("<tr>" + "".join(f"<td>{cell}</td>" for cell in row) + "</tr>" for row in rows)
    return f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>"


def write_html(output, manifest, static):
    report = manifest.get("report", {})
    themes = manifest.get("themes", [])
    screens = manifest.get("screens", {})
    findings = report.get("findings", {})
    contrast_failures = report.get("contrastFailures", {})
    pages = manifest.get("pages", [])
    titles = {"colors": "Colors", "type": "Type", "spacing": "Spacing & sizes", "icons": "Icons", "components": "Components", "inventory": "Live inventory"}

    def pictures(kind, key):
        parts = []
        for theme in themes:
            entry = screens.get(theme, {}).get(kind, {})
            if kind == "pages":
                entry = entry.get(key, {})
            if entry.get("file"):
                parts.append(f"<figure data-theme='{theme}'><img src='{escape(entry['file'])}' width='{entry.get('width', '')}' loading='lazy' alt='{escape(key)} {theme}'><figcaption>{escape(theme)} · {entry.get('width')}×{entry.get('height')}</figcaption></figure>")
        return "".join(parts)

    counts = {
        "Contrast failures": sum(len(contrast_failures.get(theme, [])) for theme in themes),
        "Fonts without a type style": sum(1 for f in findings.get("fonts", []) if not f.get("style")),
        "Margins off the scale": len(findings.get("spacing", [])),
        "Control heights off the sizes": len(findings.get("sizes", [])),
        "Icon sizes off the sizes": len(findings.get("iconSizes", [])),
        "Icons outside the theme (live)": len(findings.get("icons", [])),
        "Style sheets (live)": len(findings.get("styleSheets", [])),
        "Custom palettes (live)": len(findings.get("palettes", [])),
        "Text slips (live)": len(findings.get("texts", [])),
        "Token bypasses (check-design)": len(static["checkDesign"]),
        "Icons outside the theme (source)": len(static["icons"]),
        ".ui values off the tokens": len(static["ui"]),
        "Literal colors in C++": len(static["colors"]),
    }
    summary = "".join(f"<div class='stat{' bad' if value else ''}'><b>{value}</b><span>{escape(name)}</span></div>" for name, value in counts.items())

    sections = []
    sections.append(("summary", "Summary", f"<div class='stats'>{summary}</div><p>Generated {escape(manifest.get('generatedAt', ''))} from Kdenlive PRO {escape(manifest.get('version', ''))}. Pictures at scale {manifest.get('scale', 1)}.</p>"))
    sections.append(("window", "Main window", pictures("window", None)))
    panel_parts = []
    for theme in themes:
        for panel in screens.get(theme, {}).get("panels", []):
            panel_parts.append(f"<figure data-theme='{theme}'><img src='{escape(panel['file'])}' width='{panel.get('width', '')}' loading='lazy' alt='{escape(panel['title'])}'><figcaption>{escape(panel['title'])} ({escape(panel['name'])}) · {escape(theme)} · {panel.get('width')}×{panel.get('height')}</figcaption></figure>")
    sections.append(("panels", "Panels", "".join(panel_parts) or "<p class='empty'>No panels captured.</p>"))
    for page in pages:
        sections.append((f"page-{page}", titles.get(page, page), pictures("pages", page)))

    color_rows = [[f"<code>{escape(c['name'])}</code>" + (f" → {escape(c['alias'])}" if c.get("alias") else "")] + [f"<span class='swatch' style='background:{escape(c['values'].get(t, ''))}'></span><code>{escape(c['values'].get(t, ''))}</code>" for t in themes] + [escape(c.get("usage", ""))] for c in report.get("colors", [])]
    sections.append(("colors-table", "Color tokens", table(["Token"] + themes + ["Usage"], color_rows)))
    contrast_rows = []
    for theme in themes:
        for pair in contrast_failures.get(theme, []):
            contrast_rows.append([escape(theme), f"<code>{escape(pair['ink'])}</code>", f"<code>{escape(pair['surface'])}</code>", f"{pair['ratio']}:1", "3:1" if pair["ink"] == "ink-tertiary" else "4.5:1"])
    sections.append(("contrast", "Contrast failures", table(["Theme", "Ink", "Surface", "Ratio", "Needed"], contrast_rows)))
    type_rows = [[f"<code>{escape(s['name'])}</code>", escape(s["group"]), f"{s['px']}px / {s['lineHeight']}px", s["weight"], escape(s["letterSpacing"]), escape(s["resolvedFamily"]), escape(s.get("usage", ""))] for s in report.get("type", [])]
    sections.append(("type-table", "Type styles", table(["Style", "Group", "Size / line", "Weight", "Tracking", "Family", "Usage"], type_rows)))
    token_rows = []
    for family, tokens in report.get("tokens", {}).items():
        for token in tokens:
            value = token["value"]
            token_rows.append([escape(family), f"<code>{escape(token['name'])}</code>", escape(json.dumps(value) if isinstance(value, dict) else value), escape(token.get("usage", ""))])
    sections.append(("tokens", "Spacing, radius, size, opacity and shadow tokens", table(["Family", "Token", "Value", "Usage"], token_rows)))

    live = []
    live.append("<h3>Fonts set on widgets</h3>" + table(["Family", "Size", "Weight", "Type style", "Widgets", "Samples"], [[escape(f["family"]), f"{f['px']}px", f["weight"], f"<code>{escape(f['style'])}</code>" if f.get("style") else "<b class='bad'>none</b>", f["count"], escape(", ".join(f.get("samples", [])))] for f in findings.get("fonts", [])]))
    live.append("<h3>Margins and spacings off the scale</h3>" + table(["Pixels", "Layouts", "Samples"], [[f"{f['px']}px", f["count"], escape(", ".join(f.get("samples", [])))] for f in findings.get("spacing", [])]))
    live.append("<h3>Control heights off the sizes</h3>" + table(["Pixels", "Controls", "Samples"], [[f"{f['px']}px", f["count"], escape(", ".join(f.get("samples", [])))] for f in findings.get("sizes", [])]))
    live.append("<h3>Icon sizes off the sizes</h3>" + table(["Pixels", "Widgets", "Samples"], [[f"{f['px']}px", f["count"], escape(", ".join(f.get("samples", [])))] for f in findings.get("iconSizes", [])]))
    live.append("<h3>Icons outside the theme</h3>" + table(["Icon", "Uses"], [[f"<code>{escape(f['name'])}</code>", f["count"]] for f in findings.get("icons", [])]))
    live.append("<h3>Style sheets</h3>" + table(["Style sheet", "Widgets", "Samples"], [[f"<code>{escape(f['styleSheet'])}</code>", f["count"], escape(", ".join(f.get("samples", [])))] for f in findings.get("styleSheets", [])]))
    live.append("<h3>Custom palettes</h3>" + table(["Widget", "Count"], [[escape(f["widget"]), f["count"]] for f in findings.get("palettes", [])]))
    live.append("<h3>Text slips</h3>" + table(["Text", "Issues", "Samples"], [[escape(f["text"]), escape(", ".join(f.get("issues", []))), escape(", ".join(f.get("samples", [])))] for f in findings.get("texts", [])]))
    sections.append(("live", f"Live findings ({findings.get('widgets', 0)} widgets inspected)", "".join(live)))

    static_parts = []
    static_parts.append("<h3>check-design.py</h3>" + table(["Finding"], [[f"<code>{escape(line)}</code>"] for line in static["checkDesign"]]))
    static_parts.append("<h3>Icons the sources ask for outside the theme</h3>" + table(["Icon", "Uses", "Files"], [[f"<code>{escape(i['name'])}</code>", i["count"], escape(", ".join(i["files"]))] for i in static["icons"]]))
    static_parts.append("<h3>.ui values off the tokens</h3>" + table(["File", "Kind", "Property", "Value"], [[escape(u["file"]), escape(u["kind"]), escape(u.get("property", "")), f"<code>{escape(u['value'])}</code>"] for u in static["ui"]]))
    static_parts.append("<h3>Literal colors in C++</h3>" + table(["File", "Line", "Code"], [[escape(c["file"]), c["line"], f"<code>{escape(c['value'])}</code>"] for c in static["colors"]]))
    sections.append(("source", "Source findings", "".join(static_parts)))

    nav = "".join(f"<a href='#{sid}'>{escape(title)}</a>" for sid, title, _ in sections)
    body = "".join(f"<section id='{sid}'><h2>{escape(title)}</h2>{content}</section>" for sid, title, content in sections)
    theme_buttons = "".join(f"<button data-pick='{t}'>{escape(t)}</button>" for t in themes) + "<button data-pick='all' class='on'>both</button>"
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kdenlive PRO design review</title>
<style>
:root {{ --bg:#f5f5f7; --panel:#ffffff; --ink:#1d1d1f; --ink2:#5f5f66; --line:#00000024; --accent:#0064d1; --bad:#c4001a; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#1c1c1e; --panel:#2c2c2e; --ink:#f5f5f7; --ink2:#a1a1a6; --line:#ffffff1f; --accent:#2e95ff; --bad:#ff6961; }} }}
* {{ box-sizing: border-box; }}
body {{ margin:0; font: 13px/1.5 Inter, system-ui, sans-serif; background: var(--bg); color: var(--ink); display:flex; }}
nav {{ position: sticky; top:0; height:100vh; overflow:auto; width: 220px; padding: 16px; border-right: 1px solid var(--line); display:flex; flex-direction:column; gap:4px; flex-shrink:0; }}
nav a {{ color: var(--ink2); text-decoration:none; padding:4px 8px; border-radius:7px; }}
nav a:hover {{ background: var(--panel); color: var(--ink); }}
main {{ flex:1; padding: 16px 32px 64px; min-width:0; }}
h1 {{ font-size: 22px; margin: 8px 0 16px; }}
h2 {{ font-size: 17px; margin: 32px 0 8px; }}
h3 {{ font-size: 15px; margin: 16px 0 8px; }}
section {{ background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 16px; margin-bottom: 16px; }}
figure {{ margin: 8px 0 16px; }}
figure img {{ max-width: 100%; height:auto; border:1px solid var(--line); border-radius: 7px; background:#1c1c1e; }}
figcaption {{ color: var(--ink2); font-size: 11px; margin-top: 4px; }}
table {{ border-collapse: collapse; width:100%; font-size: 12px; }}
th, td {{ text-align:left; padding: 4px 8px; border-bottom: 1px solid var(--line); vertical-align: top; }}
th {{ color: var(--ink2); font-weight: 500; }}
code {{ font: 12px "JetBrains Mono", ui-monospace, monospace; }}
.swatch {{ display:inline-block; width:14px; height:14px; border-radius:3px; border:1px solid var(--line); vertical-align: -2px; margin-right: 6px; background-image: linear-gradient(45deg,#8883 25%,transparent 25%,transparent 75%,#8883 75%),linear-gradient(45deg,#8883 25%,transparent 25%,transparent 75%,#8883 75%); background-size:8px 8px; }}
.stats {{ display:flex; flex-wrap:wrap; gap:8px; }}
.stat {{ background: var(--bg); border-radius: 7px; padding: 8px 12px; min-width: 150px; }}
.stat b {{ display:block; font-size: 20px; }}
.stat span {{ color: var(--ink2); font-size: 11px; }}
.stat.bad b {{ color: var(--bad); }}
.bad {{ color: var(--bad); }}
.empty {{ color: var(--ink2); }}
.themes {{ display:flex; gap:2px; margin: 0 0 8px; }}
.themes button {{ font: inherit; border:1px solid var(--line); background: var(--panel); color: var(--ink); padding: 4px 10px; border-radius: 7px; cursor:pointer; }}
.themes button.on {{ background: var(--accent); color:#fff; border-color: transparent; }}
body[data-show='dark'] figure[data-theme='light'], body[data-show='light'] figure[data-theme='dark'] {{ display:none; }}
@media (max-width: 800px) {{ body {{ flex-direction: column; }} nav {{ position: static; width:auto; height:auto; flex-direction: row; flex-wrap: wrap; }} main {{ padding: 16px; }} }}
</style></head>
<body data-show="all"><nav><b>Design review</b>{nav}</nav><main>
<h1>Kdenlive PRO design review</h1>
<div class="themes">{theme_buttons}</div>
{body}
</main>
<script>
document.querySelectorAll('.themes button').forEach(b => b.addEventListener('click', () => {{
  document.body.dataset.show = b.dataset.pick;
  document.querySelectorAll('.themes button').forEach(o => o.classList.toggle('on', o === b));
}}));
</script></body></html>"""
    (output / "index.html").write_text(page)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=os.environ.get("KDENLIVE_BINARY") or str(ROOT / "build" / "bin" / "kdenlive"), help="the built editor")
    parser.add_argument("--output", default=str(ROOT / "build" / "design-review"), help="folder for the pictures, review.json, report.json and index.html")
    parser.add_argument("--project", help="a .kdenlive file to open for the window and panel pictures (default: a generated fixture)")
    parser.add_argument("--scale", type=float, default=1.0, help="device pixel ratio of the pictures (the editor renders at 1 and this is recorded only)")
    parser.add_argument("--window", default="1600x1000", help="main window size for the pictures")
    parser.add_argument("--settle", type=int, default=2500, help="milliseconds to wait after the window is up before capturing")
    parser.add_argument("--skip-run", action="store_true", help="reuse an existing review.json in the output folder and only rebuild the audits and index.html")
    parser.add_argument("--strict", action="store_true", help="exit 1 when a source audit or a contrast check fails")
    args = parser.parse_args()

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    width, height = (int(part) for part in args.window.lower().split("x"))
    if not args.skip_run:
        binary = Path(args.binary)
        if not binary.exists():
            print(f"No editor at {binary}; build first or pass --binary", file=sys.stderr)
            return 2
        fixture_folder = Path(tempfile.mkdtemp(prefix="kdenlive-design-fixture-"))
        project = Path(args.project).resolve() if args.project else make_fixture(fixture_folder)
        ok = run_editor(binary, output, project, args.scale, (width, height), args.settle)
        shutil.rmtree(fixture_folder, ignore_errors=True)
        if not ok:
            return 2
    manifest = json.loads((output / "review.json").read_text())
    static = {"checkDesign": load_check_design().collect(), "icons": icon_audit(), "ui": ui_audit(), "colors": color_audit()}
    (output / "report.json").write_text(json.dumps({"manifest": manifest, "source": static}, indent=1))
    write_html(output, manifest, static)
    contrast = sum(len(entries) for entries in manifest.get("report", {}).get("contrastFailures", {}).values())
    print(f"Design review: {output / 'index.html'}")
    print(f"  contrast failures {contrast}, check-design {len(static['checkDesign'])}, icons outside the theme {len(static['icons'])}, .ui values {len(static['ui'])}, literal colors {len(static['colors'])}")
    if args.strict and (contrast or static["checkDesign"] or static["icons"] or static["ui"] or static["colors"]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
