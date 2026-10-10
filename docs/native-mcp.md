# Native MCP API

The editor serves MCP directly over Streamable HTTP. No companion server,
Node runtime or D-Bus bridge is required for live editing.

## Enable and connect

1. Open **Settings → Configure Kdenlive → MCP API**.
2. Enable the API, choose a port, and click Apply. The default is 8765.
3. Check that the status says `Listening on http://127.0.0.1:8765/mcp`.
4. Pick your client's format under **Client configuration** and click **Copy**:
   generic `mcpServers` JSON (Claude Desktop, Cursor and most clients), a
   `claude mcp add --scope user` command for Claude Code, a Codex `config.toml` entry, or the
   bare URL and header. Add it to your client and reconnect.
5. Open and save a project in Kdenlive. MCP edits act on its visible sequence.

Import files from the project's folder or set an additional media folder here.
The API resolves symlinks before checking these folders. Existing project media
stays usable. Removing a bin asset never deletes the original file.

## Access and security

By default the server binds only to IPv4 localhost and needs no token. It
rejects any request carrying an `Origin` header, so web pages cannot call it,
and any `Host` other than `127.0.0.1` or `localhost`, which defeats DNS
rebinding.

**Require access token** additionally blocks other users and programs on the
same computer. The random bearer token is stored in the application's config
directory as `mcp-token` with owner-only permissions, and copied configurations
include it.

**Accept connections from other devices on the network** binds to all
interfaces, always requires the token and accepts any `Host`. The status shows
the LAN address that copied configurations use. Traffic is plain HTTP, so the
token and project data are visible on the network: use it only on trusted
networks. For remote access, use an SSH tunnel to the localhost port or an HTTPS
reverse proxy instead.

Regenerating the token disconnects clients; copy the new configuration
afterwards. Changing the port or access settings, or disabling the API, also
ends MCP connections. A busy port is reported in settings rather than silently
replaced.

## Available tools

Parameters below are in addition to `sessionId`, `expectedRevision` and
`requestId`, which every editing tool requires. Clip and track IDs are integers
from `desktop_state`; bin IDs are numeric strings.

| Tools | Parameters | Purpose |
| --- | --- | --- |
| `desktop_capabilities` | none | Read capabilities, including the state section names and the project's marker categories |
| `desktop_state` | optional `include` (section names), `trackId`, `range` (`start`, `end`) | Read the visible sequence, bin and history; see [State snapshot](#state-snapshot) |
| `desktop_frame_capture` | `position`, optional `width` (64 to 1920, default 540) | Return one rendered frame as a PNG image without moving the playhead |
| `desktop_effect_list` | optional `clipId` or `binId`, optional `query` | List a clip's effects with parameters and `keyframeOrigin`, and/or search the effect catalog |
| `desktop_title_read` | `binId` | Read a title clip's canvas, the project frame size and its items |
| `desktop_render_status` | none | Follow the progress of renders started by `desktop_render` |
| `desktop_marker_export` | `format` (`json`, `csv`, `kdenlive`), optional `binId` | Return the sequence guides, or a bin clip's markers, as text |
| `desktop_media_import`, `desktop_media_remove` | `path`; `binId` | Add files to the bin or remove unused assets |
| `desktop_clip_insert`, `desktop_clip_remove` | `binId`, `trackId`, `position`, `sourceIn`, `sourceOut`, `media`; `clipId` | Add or remove timeline clips |
| `desktop_media_replace` | `binId`, `replacementBinId` | Replace an asset while preserving timeline ranges |
| `desktop_clip_move`, `desktop_clip_trim` | `clipId`, `trackId`, `position`; `clipId`, `duration`, `edge` | Move clips and trim their edges |
| `desktop_audio_envelope`, `desktop_track_rename` | `clipId`, `fadeIn`, `fadeOut`, `gainDb`; `trackId`, `name` | Add audio fades/gain and name tracks |
| `desktop_project_save`, `desktop_undo`, `desktop_redo` | none | Save and use shared native history |
| `desktop_project_save_as` | `path` | Save a copy and keep editing it |
| `desktop_project_profile` | `width`, `height` (even, 16 to 8192) | Change the frame size, e.g. 1080x1920 vertical; keeps the frame rate and is not undoable |
| `desktop_clip_reframe` | `clipId`, `mode` (`fill`/`fit`), optional `focusX`, `focusY`, `endFocusX`, `endFocusY`, `zoom` | Fill or fit a clip in the frame with a Transform effect, with focus point, pan and zoom |
| `desktop_effect_add`, `desktop_effect_set`, `desktop_effect_remove` | `clipId` or `binId`; `effectId` and optional `params`; `index` and `params`; `index` | Edit effects on timeline clips or bin clips with MLT parameter strings |
| `desktop_title_edit` | `binId`, optional `width`, `height`, `items` (`index`, `text`, `x`, `y`, `fontPixelSize`, `alignment`) | Edit a title clip's canvas size and text items |
| `desktop_marker_add` | `position`, optional `duration`, `comment`, `category`, `binId` | Add a guide, or with `binId` a clip marker; replaces a marker at the same frame |
| `desktop_marker_edit` | `position`, optional `binId`, then any of `newPosition`, `duration`, `comment`, `category` | Change one guide or clip marker |
| `desktop_marker_remove` | optional `binId`, and `position`, `all: true`, or `category` and/or `range` (`start`, `end`) | Remove one or many markers in one Undo step |
| `desktop_marker_import` | `format`, `text`, optional `binId` | Add markers from exported text in one Undo step |
| `desktop_render` | `path`, optional `preset` | Render the active sequence with a preset |
| `desktop_batch` | `commands` (1 to 200 `desktop_apply` commands) | Apply edits as one Undo step, rolled back on the first failure |
| `desktop_apply` | `request` with the edit fields and one `command` | Submit any supported operation using the common request envelope |

Tool annotations follow the MCP hints. The seven reading tools are
`readOnlyHint`. `desktop_media_import`, `desktop_clip_insert`,
`desktop_audio_envelope`, `desktop_project_save_as`, `desktop_effect_add` and
`desktop_render` only add content and are not `destructiveHint`; every other
editing tool is. Repeating `desktop_clip_insert`, `desktop_effect_add`,
`desktop_effect_remove`, `desktop_render`, `desktop_undo`, `desktop_redo`,
`desktop_batch` or `desktop_apply` with new edit fields has a further effect,
so they are not `idempotentHint`. Independently of the hints, an identical retry
with the same `requestId` returns the saved result.

Read `desktop_state` before editing. Each edit needs its `sessionId`, the
`expectedRevision`, and a unique `requestId`. Frame values use the project FPS;
`sourceOut` is exclusive. Import and replacement load asynchronously. Poll state
until the affected bin asset is ready before using it.

## Markers and guides

Without `binId`, marker tools act on the active sequence's guides; with
`binId` they act on that bin clip's markers, at frames relative to the clip.
Sequence bin clips are refused with `SEQUENCE_PROTECTED`, because their markers
are the sequence guides. `category` is an index or a name (exact, then
case-insensitive) from `markerCategories` (`index`, `name`, `color`) in
`desktop_capabilities`, which reflects the open project; omitted, it is
`defaultMarkerCategory`. A `duration` above 0 makes a range marker. Kdenlive
keys markers by frame: `desktop_marker_add` at an occupied frame replaces that
marker's comment, category and duration, as the native model does, and reports
`replaced: true`. Kdenlive reads an empty comment back as `Marker`.

Results carry `target` (`guides` or `clip`), `binId` for clip markers, and
`marker` (add, edit, plus `previous` and `changed` for edit), `removed` and
`count` (remove), or `imported` and `replaced` (import), in the
[snapshot](#state-snapshot) marker shape. Each call is one Undo step, labelled
for example `Add guide`, `Move guide`, `Remove 3 guides` or `Import 5 clip
markers` after Kdenlive's `hh:mm` prefix. Removal by `category` and/or `range`
takes markers whose position is in `[start, end)`. The marker commands can run
inside `desktop_batch`.

`desktop_marker_export` returns `text`, so it writes no file: `json` is the
snapshot marker array; `csv` has a header row
`position,timecode,duration,category,categoryName,color,comment`, with the
project `HH:MM:SS:FF` timecode and RFC 4180 quoting; `kdenlive` is
`MarkerListModel::toJson`, the format of Kdenlive's own guide export
(`pos`, `comment`, `type`, `duration`). `desktop_marker_import` reads all three.
CSV columns are matched by name; it needs `position` (frames) or `timecode`,
and reads `category` or `categoryName`. JSON imports ignore unknown keys. Every
entry is validated before anything changes. Errors: `UNKNOWN_MARKER` (nothing
at `position`, or no marker matches the filter), `UNKNOWN_CATEGORY`,
`MARKER_EXISTS` (editing onto an occupied frame), and `INVALID_ARGUMENTS` for
positions, durations and ranges that are not whole frames or leave the sequence
(or clip), and for unreadable import text. A changed position or duration must
fit; an existing guide past a shortened sequence can still be edited in place
or removed. Malformed fields fail with `INVALID_COMMAND`.

## State snapshot

`desktop_state` and every successful edit return the same snapshot shape.
Edit results embed it as `state`, always with the default sections. Frames are
project frames; ranges end exclusively.

These fields are always present: `sessionId`, `revision`, `sequenceId` (the
active sequence UUID), `documentUrl`, `modified`, `fps` (`numerator`,
`denominator`), `profile` (`width`, `height`, `description`, `duration`),
`playhead`, `activeTrackId`, `undo` (`index`, `canUndo`, `canRedo`,
`undoText`, `redoText`), `sections` (the sections returned) and `counts`
(`tracks`, `clips`, `compositions`, `markers`, `subtitles`, `bin` for the whole
sequence, whatever the filters; `markers` counts guides only). A filtered read also returns `filter`.

| Section | Default | Adds |
| --- | --- | --- |
| `tracks` | yes | `tracks[]`: `id`, `name`, `tag` (V1, A1...), `type` (`video`/`audio`), `audio`, `locked`, `muted` (audio tracks), `hidden` (video tracks), `active` |
| `clips` | yes | `tracks[].clips[]` sorted by position: `id`, `binId`, `name`, `type`, `position`, `duration`, `sourceIn`, `sourceOut`, `speed`, `enabled`, `effectCount`, `grouped`, `groupId`, `linkedClipId` (A/V partner), `mixes[]` (`edge` `start`/`end`, `position`, `duration`, and for `start` the mix cut `offset`); `tracks[].gaps[]` (`position`, `duration`) between clips |
| `compositions` | yes | `tracks[].compositions[]`: `id`, `compositionId`, `name`, `position`, `duration`, `aTrack` and `bTrack` (MLT indexes, 0 is the black background), `aTrackId` (null for the background), `forcedATrack`, `grouped`, `groupId` |
| `markers` | yes | `markers[]` (sequence guides): `position`, `duration` (0 for a point), `comment`, `category`, `categoryName`, `color` |
| `bin` | yes | `bin[]`: `id`, `name`, `type`, `parentId` (folder, null at the root), `url` (media path, empty for generated clips), `ready`, `inUse`, `duration`, and `markers[]` (clip markers, same shape as guides, frames relative to the clip) only when the clip has any; `folders[]`: `id`, `name`, `parentId` |
| `sequences` | yes | `sequences[]`: `id` (UUID), `binId`, `name`, `active`, `open` |
| `subtitles` | no | `subtitles[]` sorted by start: `id`, `layer`, `start`, `end`, `text`; empty when the sequence has no subtitles |
| `effects` | no | `tracks[].clips[].effects[]`: `effectId`, `name`, `enabled`, in stack order; implies `clips` |
| `media` | no | `width`, `height`, `fps`, `hasVideo`, `hasAudio` on ready bin items (null when not applicable); implies `bin` |

Clip types are `av`, `video`, `audio`, `image`, `color`, `title`, `text`,
`slideshow`, `playlist`, `animation`, `sequence` or `other`. `clips`,
`compositions` and `effects` imply `tracks`. `include` replaces the default set,
so `["subtitles"]` returns only the always-present fields and subtitles.
`trackId` keeps one track. `range` keeps clips, gaps, compositions, markers and
subtitles that overlap it. Unknown sections or keys and an empty range fail
with `INVALID_ARGUMENTS`; an unknown track fails with `UNKNOWN_TRACK`.

Retry an uncertain edit only with its identical request ID and arguments. The
engine keeps 64 receipts across HTTP reconnections and port changes. Opening a
different document/sequence or restarting invalidates the editing session.
Manual changes advance the revision and share Undo with MCP edits.

The server implements the 2025-11-25 MCP lifecycle, with compatible 2025-06-18
and 2025-03-26 negotiation. It returns JSON responses, accepts notifications
with HTTP 202, and returns HTTP 405 for GET. It supports 32 sessions with a
30-minute idle expiry and enforces HTTP body/header limits through Qt.

Save-as and render outputs follow the same folder policy as imports, and
neither overwrites an existing file. Effect keyframes count frames from the
`keyframeOrigin` that `desktop_effect_list` reports. A profile change rebuilds
bin producers: filters that Kdenlive does not list as effects are dropped, so
save a copy first.

This version does not expose project creation, track creation, downloads or the
companion's offline project tools. Open/create projects in the GUI. Native state
describes the active sequence, the project bin with its clip markers, and the
list of sequences; keyframes and effect parameters are read with their own tools.

## Build and install

Follow the upstream development dependencies, plus **Qt 6.11 or newer
HttpServer and WebSockets**, matching the installed Qt version.

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DKDENLIVE_MCP_API=ON -DBUILD_MCP_TESTING=ON \
  -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build --parallel 2
ctest --test-dir build -R mcp_protocol --output-on-failure
cmake --install build
bash packaging/live-mcp/install-user.sh "$PWD/install"
```

If the two optional Qt modules come from a separate matching prefix, pass its
library directory as the installer's second argument. They are copied into the
private installation. The launcher uses that installed library directory, so the
checkout and build dependencies are unnecessary at runtime. Other Qt/KDE/MLT
dependencies remain system libraries. API support is disabled at build time by
default, and the runtime setting also defaults to disabled.

`KDENLIVE_LIVE_BRIDGE=ON` optionally includes the legacy D-Bus adapter. Launch
with `KDENLIVE_MCP_BRIDGE=1` to expose it. The HTTP server uses the same editing
engine but does not need the legacy adapter enabled.

## Tests

The protocol tests can also build separately with `cmake -S tests/mcp -B
mcp-test-build`. They cover authentication, origin rejection, JSON-RPC lifecycle,
request limits, optional and network token checks, client configuration formats, token rotation, port conflicts, reconfiguration and shutdown.

The SDK acceptance test in `tests/mcp/acceptance.mjs` launches a disposable editor
and exercises native operations through HTTP. Node and the MCP SDK are test-only
dependencies. See its header for invocation. It never edits your open project.

**Acceptance checks.** The run calls all 33 tools. It checks the catalog against
the expected tool names and annotations, then reads the fixture's state:
track type, lock and mute flags, a linked and grouped audio/video pair, a gap,
a dissolve composition, two guides, two subtitles, bin folders and types, and
the sequence list; it checks `include`, `trackId` and `range` scoping, the
media section and rejected arguments. It reads the marker categories and a
clip marker from the fixture; adds point and range guides by category index and
name, replaces, edits, moves and removes them with undo/redo; removes many by
`all`, `category` and `range` in one Undo step that one `desktop_undo` restores;
round-trips json, csv and kdenlive exports through import; runs marker commands
in a batch; adds, edits, exports and removes bin-clip markers; and checks the
marker error codes. It then tests inserting, moving,
trimming and removing clips with undo/redo; receipt replay, `REVISION_CONFLICT` and
`REQUEST_ID_REUSED`; the import folder policy, including a symlink escape;
importing, removing and replacing media; the audio envelope and track rename;
saving and reopening, which yields a new session. It then captures frames and
checks the PNG image block and its size; searches the effect catalog; adds,
sets, removes and undoes an effect and reads it in the `effects` section, with
unknown effects, indexes and parameters rejected; reads and edits a title clip from the fixture, with undo/redo; runs
`desktop_apply`; applies a batch as one Undo step and checks that a failing batch
leaves the timeline, bin and history unchanged; saves a copy, with a path outside
the allowed folders refused; switches to 1080x1920 at the same frame rate; pans
and fits a clip with `desktop_clip_reframe`; resizes the title canvas; and
renders the 1080x1920 sequence, polling `desktop_render_status` until the file
is finished, with bad paths, an unknown preset and a second render refused. It
takes about 30 seconds.

Codex's HTTP configuration fields are documented in its
[configuration reference](https://developers.openai.com/codex/config-reference).
