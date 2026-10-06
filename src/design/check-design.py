#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Report interface colors and fonts that bypass the design tokens.

    python3 src/design/check-design.py [--strict]

Lists hard coded colors and system palette lookups in QML, and system font lookups in C++.
Content colors (scopes, titler, thumbnails, user data) are allowed with a `design:allow`
comment on the same line. With --strict the script exits non zero when it finds any, for CI.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent

QML_DIRS = ["timeline2/view/qml", "monitor/view", "qml"]
QML_RULES = [
    (re.compile(r"""(color|Color)\s*:\s*['"](#[0-9a-fA-F]{3,8}|red|white|black|green|blue|yellow|orange|purple|gray|grey)['"]"""), "hard coded color, use K.Design.colors"),
    (re.compile(r"activePalette\.(text|window|base|highlight|highlightedText|windowText|alternateBase)\b"), "system palette, use K.Design.colors"),
]
CPP_RULES = [
    (re.compile(r"QFontDatabase::systemFont\(QFontDatabase::(SmallestReadableFont|FixedFont)\)"), "system font, use UiUtils::smallFont() or fixedFont()"),
    (re.compile(r"setStyleSheet\(QStringLiteral\(\"[^\"]*(#[0-9a-fA-F]{3,8}|rgb\(|:\s*red)"), "hard coded style sheet color, use DesignPaint::badgeStyleSheet()"),
]
CPP_SKIP = {"utils/uiutils.cpp"}


def scan(path, rules):
    findings = []
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if "design:allow" in line or line.lstrip().startswith("//"):
            continue
        for pattern, message in rules:
            if pattern.search(line):
                findings.append(f"{path.relative_to(SRC)}:{number}: {message}")
    return findings


def main():
    findings = []
    for directory in QML_DIRS:
        for path in sorted((SRC / directory).glob("*.qml")):
            findings += scan(path, QML_RULES)
    for path in sorted(SRC.rglob("*.cpp")):
        if str(path.relative_to(SRC)) not in CPP_SKIP:
            findings += scan(path, CPP_RULES)
    print("\n".join(findings) if findings else "No design token bypasses found")
    if findings and "--strict" in sys.argv:
        sys.exit(1)


if __name__ == "__main__":
    main()
