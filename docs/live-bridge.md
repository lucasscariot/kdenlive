# Optional live editing bridge

This local extension adds an explicitly enabled D-Bus API to Kdenlive 26.08.1.
Build with `-DKDENLIVE_LIVE_BRIDGE=ON` and launch with
`KDENLIVE_MCP_BRIDGE=1`. The default build and launch leave the API disabled.
The source patch is based on official KDE commit
`55e16e85cd9a9c6e032cd27a621137b4da881a7c`.

## Build and install on Linux

Install the development dependencies in the upstream [build guide](../dev-docs/build.md),
including CMake, a C++ compiler, ECM, Qt 6, KDE Frameworks 6 and MLT 7.
Then build this branch with the bridge enabled:

```bash
git clone --branch feat/mcp-live-bridge https://github.com/lucasscariot/kdenlive.git
cd kdenlive
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DKDENLIVE_LIVE_BRIDGE=ON -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build --parallel 2
cmake --install build
bash packaging/live-mcp/install-user.sh "$PWD/install"
```

On Omarchy, open **Kdenlive Live MCP** from the app launcher. The terminal command
is `~/.local/bin/kdenlive-live`, which also accepts a `.kdenlive` project path.
Installation copies the built application to `~/.local/opt/kdenlive-live`, so
the original checkout and build folder are not needed to launch it.
To enable the legacy bridge, launch with `KDENLIVE_MCP_BRIDGE=1`. The launcher uses `kdenlive-live` subdirectories inside
the user's XDG config, data and cache directories. The system Kdenlive package
remains installed. The build still needs the host's Qt, KDE
and MLT libraries; rebuild after incompatible system-library updates.

Re-run the install command after rebuilding to update the app. To uninstall,
remove `~/.local/opt/kdenlive-live`, `~/.local/bin/kdenlive-live` and
`org.kde.kdenlive.live.desktop` from your XDG applications directory.
Settings remain in the separate XDG directories until removed explicitly.

The MCP client needs the companion `desktop_instances`, `desktop_state` and
`desktop_apply` tools and access to the same user's D-Bus session. This repository
also offers a [native HTTP MCP server](native-mcp.md), which connects directly
without the companion or D-Bus bridge.

## D-Bus interface

The object `/org/kde/kdenlive/LiveBridge` uses interface
`org.kde.kdenlive.LiveBridge1` on Kdenlive's existing session-bus service.
Methods `capabilities()` and `state()` return JSON strings. `apply(string)` takes
a JSON request and returns a JSON result. `changed(string)` reports the active
session and revision. The session bus restricts access to the local login session;
this is not an authenticated network service.

## Commands

```json
{
  "sessionId": "from state()",
  "expectedRevision": 1,
  "requestId": "unique-for-this-operation",
  "command": {
    "type": "insert",
    "binId": "2",
    "trackId": 1,
    "position": 360,
    "sourceIn": 30,
    "sourceOut": 90,
    "media": "video"
  }
}
```

Supported commands:

- `import`: absolute local `path`. Returns `binId`; poll state until that bin
  clip is ready. Reimporting the same canonical path returns the existing ID.
- `remove_asset`: `binId`. Removes an unused asset from the bin with native
  undo. Rejects assets used in any sequence and protects sequences themselves.
  The original file stays on disk. Bin state exposes `inUse`.
- `remove_clip`: `clipId`. Removes one ungrouped timeline clip without rippling.
  Rejects locked tracks; retains the bin asset and source file. Native undo
  restores its original position and effects.
- `replace_media`: `binId` and `replacementBinId` of two ready bin clips.
  Uses native Replace Clip, preserving timeline positions, ranges and effects
  for every instance of the target. The replacement must have matching audio/video
  streams and be at least as long as the original media. Import it first.
- `save`: save the current project to its existing writable local file. All
  bin clips must be ready. Saving does not add a history entry.
- `audio_envelope`: audio `clipId`, `fadeIn` and `fadeOut` in project frames,
  and `gainDb` from -60 to 0. Adds a native keyframed volume effect, rejecting
  clips that already have one. Fades must not overlap. Undo removes the effect.
- `rename_track`: `trackId` and `name`, using native undoable track renaming.
- `insert`: the fields above, with `media` equal to `video` or `audio`.
- `move`: `clipId`, `trackId`, `position`.
- `trim`: `clipId`, `duration`, `edge` equal to `left` or `right`.
- `undo` and `redo`: no additional fields. These use the shared editor history.

Frames use the project profile. Insertion's source end is exclusive. Readback of
speed-adjusted existing clips reports their native producer frame range and speed;
do not interpret that range as original-media time without applying the speed.
Trim reports its actual resulting duration. IDs are native runtime IDs and may
change when a project is reopened. Operations preserve native grouping behavior.

The bridge runs native editing operations on the GUI thread and refreshes the
timeline/monitor. It checks session and revision immediately before editing.
Model content changes and native undo-stack activity advance the revision.
Thumbnail and selection updates are ignored. Revision increments can exceed one
per command. The response contains a snapshot after successful application;
display painting and media decoding happen asynchronously afterwards.

## Retry and limits

The latest 64 requests retain their original responses. An identical request ID
and payload returns the saved result without repeating the edit. Reusing the ID
with different content fails. An evicted successful request cannot run again with
its old revision. Receipts are in memory; process restart, document replacement
or active-sequence change invalidates the session. On a lost response, retry the
identical request before deciding what happened.

The bridge exposes no batch, title, general effect-stack or rendering command.
State covers active-sequence tracks/clips and the project bin, not
the complete native document. It does not synchronize an offline project model.
It rejects edits while a modal dialog or mouse drag is active. Native validation
handles track locks, collisions and groups. Some trim requests may be constrained
by Kdenlive; use the returned `actualDuration` and state.

The companion TypeScript MCP exposes `desktop_instances`, `desktop_state` and
`desktop_apply`. Its mutation adapter additionally requires the open project to
be saved inside a configured workspace root. It currently reads state on demand;
D-Bus notifications are not forwarded as MCP subscription events.
