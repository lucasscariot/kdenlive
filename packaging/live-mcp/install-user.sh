#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ! -x "$1/bin/kdenlive" || ! -d "$1/share/kdenlive" ]]; then
  echo "Usage: $0 <installed-CMake-prefix> [matching-Qt-library-directory]" >&2
  exit 2
fi

source_tree=$(realpath -- "$1")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
live_prefix="$HOME/.local/opt/kdenlive-live"
desktop_dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
desktop_file="$desktop_dir/org.kde.kdenlive.live.desktop"
command_path="$HOME/.local/bin/kdenlive-live"

# Desktop-entry quoting needs additional escaping for these unusual home paths.
case "$HOME" in
  *[\$\`\"\\%]*|*$'\n'*|*$'\r'*) echo 'Unsupported characters in HOME' >&2; exit 2 ;;
esac
if [[ "$source_tree" == "$live_prefix" || "$source_tree" == "$live_prefix/"* ]]; then
  echo 'The source installation must be outside the destination' >&2
  exit 2
fi
if [[ -e "$command_path" && ! -L "$command_path" ]]; then
  echo "Refusing to replace an existing command: $command_path" >&2
  exit 1
fi

mkdir -p "$live_prefix" "$HOME/.local/bin" "$desktop_dir"
# Replacing directory entries also allows updating an executable that is running.
cp -a --remove-destination "$source_tree/." "$live_prefix/"
if [[ $# == 2 ]]; then
  # Bundle only the optional modules, using the same Qt version as the system.
  mkdir -p "$live_prefix/lib"
  for module in HttpServer WebSockets; do
    library="$2/libQt6$module.so.6"
    [[ -f "$library" ]] || { echo "Missing Qt module: $library" >&2; exit 1; }
    cp -L --remove-destination "$library" "$live_prefix/lib/libQt6$module.so.6"
  done
fi
if LD_LIBRARY_PATH="$live_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$live_prefix/bin/kdenlive" | grep -q 'not found'; then
  echo 'Installed editor has missing runtime dependencies' >&2
  exit 1
fi
install -m 755 "$script_dir/kdenlive-live" "$live_prefix/bin/kdenlive-live"
ln -sfn "$live_prefix/bin/kdenlive-live" "$command_path"
cat > "$desktop_file" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Kdenlive Live MCP
GenericName=Video Editor
Comment=Custom Kdenlive with live MCP editing
Exec="$command_path" %F
Icon=$live_prefix/share/icons/hicolor/256x256/apps/kdenlive.png
Terminal=false
StartupNotify=true
StartupWMClass=org.kde.kdenlive.live
Categories=Qt;KDE;AudioVideo;AudioVideoEditing;
MimeType=application/x-kdenlive;
Keywords=video;editing;MCP;
DESKTOP
if command -v desktop-file-validate >/dev/null; then
  desktop-file-validate "$desktop_file"
fi
if command -v update-desktop-database >/dev/null; then
  update-desktop-database "$desktop_dir"
fi
printf 'Installed %s\nLauncher: %s\n' "$live_prefix" "$desktop_file"
