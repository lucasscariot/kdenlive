#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Generate the Kdenlive Pro icon themes from Lucide icons and the design tokens.

Each freedesktop or Kdenlive icon name below maps to one Lucide icon, so one meaning has one
drawing across the app. Icons are drawn at a 1.5px stroke in the theme's icon ink; names not
listed fall back to Breeze through the theme's Inherits key.

    python3 src/design/generate-icon-theme.py <path to lucide-static/icons>

Lucide is ISC licensed (https://lucide.dev).
"""

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "icons"

ICONS = {
    # Transport
    "media-playback-start": "play",
    "media-playback-pause": "pause",
    "media-playback-stop": "square",
    "media-seek-backward": "rewind",
    "media-seek-forward": "fast-forward",
    "media-skip-backward": "skip-back",
    "media-skip-forward": "skip-forward",
    "media-record": "circle-dot",
    "process-stop": "circle-stop",
    # Editing
    "edit-cut": "scissors",
    "edit-copy": "copy",
    "edit-paste": "clipboard-paste",
    "edit-delete": "trash-2",
    "edit-clear": "x",
    "edit-undo": "undo-2",
    "edit-redo": "redo-2",
    "edit-select": "mouse-pointer-2",
    "edit-find": "search",
    "edit-find-replace": "replace",
    "document-edit": "pencil",
    "draw-text": "type",
    "transform-move": "move",
    "transform-crop": "crop",
    "snap": "magnet",
    "link": "link",
    "tag": "tag",
    "bookmark-new": "bookmark-plus",
    "favorite": "star",
    "speedometer": "gauge",
    "measure": "ruler",
    # Keyframes
    "keyframe": "diamond",
    "keyframe-add": "diamond-plus",
    "keyframe-remove": "diamond-minus",
    "keyframe-previous": "step-back",
    "keyframe-next": "step-forward",
    # Files
    "document-new": "file-plus",
    "document-open": "folder-open",
    "document-save": "save",
    "document-save-as": "save-all",
    "document-import": "import",
    "document-export": "share",
    "folder": "folder",
    "folder-new": "folder-plus",
    "folder-videos": "folder",
    "video-x-generic": "film",
    "audio-x-generic": "music",
    "image-x-generic": "image",
    "insert-image": "image",
    # Lists and views
    "list-add": "plus",
    "list-remove": "minus",
    "view-list-icons": "layout-grid",
    "view-list-details": "list",
    "view-list-tree": "list-tree",
    "view-filter": "filter",
    "view-refresh": "refresh-cw",
    "view-media-playlist": "list-video",
    "view-preview": "monitor-play",
    "view-split-left-right": "square-split-horizontal",
    "view-split-top-bottom": "square-split-vertical",
    "zoom-in": "zoom-in",
    "zoom-out": "zoom-out",
    "zoom-fit-best": "maximize-2",
    "zoom-original": "search-check",
    "go-next": "chevron-right",
    "go-previous": "chevron-left",
    "go-down": "chevron-down",
    "go-up": "chevron-up",
    # State
    "visibility": "eye",
    "kdenlive-show-video": "eye",
    "kdenlive-hide-video": "eye-off",
    "audio-volume-high": "volume-2",
    "audio-volume-medium": "volume-1",
    "audio-volume-muted": "volume-x",
    "audio-off": "volume-x",
    "lock": "lock",
    "unlock": "lock-open",
    # Panels and app
    "application-menu": "menu",
    "configure": "settings",
    "settings-configure": "settings",
    "tools-wizard": "wand-sparkles",
    "view-media-equalizer": "sliders-vertical",
    "document-properties": "panel-right",
    "composite-track-on": "layers",
    "color-management": "palette",
    "help-contents": "circle-help",
    "help-hint": "info",
    "dialog-ok": "check",
    "dialog-cancel": "ban",
    "dialog-close": "x",
    "window-close": "x",
    # Timeline toolbar and status bar
    "cursor-arrow": "mouse-pointer-2",
    "distribute-horizontal-x": "move-horizontal",
    "kdenlive-ripple": "arrow-left-right",
    "kdenlive-slip": "fold-horizontal",
    "composite-track-preview": "blend",
    "timeline-overwrite": "arrow-down-to-line",
    "timeline-insert": "between-horizontal-end",
    "timeline-extract": "panel-bottom-close",
    "timeline-lift": "panel-bottom-open",
    "timeline-use-zone-on": "frame",
    "timeline-use-zone-off": "frame",
    "add-subtitle": "captions",
    "kdenlive-show-markers": "message-square-text",
    "kdenlive-show-audiothumb": "audio-waveform",
    "waveform": "audio-waveform",
    "kdenlive-show-videothumb": "film",
    "zoom-fit-width": "maximize-2",
}

# Icon ink per theme: the design's secondary ink, the color of icons at rest
THEMES = {"dark": "KdenlivePro-dark", "light": "KdenlivePro-light"}


def icon_ink(theme):
    tokens = json.loads((HERE / "tokens.json").read_text())
    for token in tokens["color"]["tokens"]:
        if token["name"] == "ink-secondary":
            return token["value"][theme]
    raise KeyError("ink-secondary")


def recolor(svg, ink):
    svg = re.sub(r"<!--.*?-->\s*", "", svg, flags=re.S)
    svg = svg.replace('stroke="currentColor"', f'stroke="{ink}"')
    return svg.replace('stroke-width="2"', 'stroke-width="1.5"')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    source = Path(sys.argv[1])
    for theme, name in THEMES.items():
        ink = icon_ink(theme)
        target = OUTPUT / name / "scalable"
        target.mkdir(parents=True, exist_ok=True)
        for icon, lucide in ICONS.items():
            (target / f"{icon}.svg").write_text(recolor((source / f"{lucide}.svg").read_text(), ink))
        inherits = "breeze-dark,breeze,hicolor" if theme == "dark" else "breeze,hicolor"
        (OUTPUT / name / "index.theme").write_text(
            "[Icon Theme]\n"
            f"Name={name}\n"
            "Comment=Kdenlive Pro icons, generated from Lucide by src/design/generate-icon-theme.py\n"
            f"Inherits={inherits}\n"
            "Directories=scalable\n\n"
            "[scalable]\nSize=16\nMinSize=8\nMaxSize=512\nType=Scalable\n"
        )
        print(f"wrote {len(ICONS)} icons to {name}")


if __name__ == "__main__":
    main()
