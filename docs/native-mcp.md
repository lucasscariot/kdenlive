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
5. Open a project in Kdenlive. For an unsaved project, configure an additional
   media folder and use `desktop_project_save_as` inside that folder for the
   first save. MCP edits act on its visible sequence.

For Codex, register the running editor directly instead of writing an HTTP
client:

```sh
codex mcp add kdenlive --url http://127.0.0.1:8765/mcp
```

When access tokens are enabled, make the token available to Codex through an
environment variable and add `--bearer-token-env-var KDENLIVE_MCP_TOKEN`.
Use the URL shown in settings if the port differs, then reconnect the client.

### Keeping the tools available: the stdio shim

Agent harnesses register MCP servers when a session starts. The HTTP endpoint
exists only while the editor runs, so a session started first has no
`desktop_*` tools and agents fall back to hand-written HTTP clients.
`packaging/live-mcp/kdenlive-mcp-stdio.py` (installed as
`~/.local/opt/kdenlive-live/bin/kdenlive-mcp-stdio`) is a stdio MCP server
that forwards every request to the endpoint and, when nothing answers,
launches the editor (`kdenlive-live` by default) and waits for it. Register
it as a stdio server:

```sh
codex mcp add kdenlive -- ~/.local/opt/kdenlive-live/bin/kdenlive-mcp-stdio
claude mcp add --scope user kdenlive -- ~/.local/opt/kdenlive-live/bin/kdenlive-mcp-stdio
```

`KDENLIVE_MCP_URL`, `KDENLIVE_MCP_TOKEN`, `KDENLIVE_MCP_LAUNCH` (empty disables
launching) and `KDENLIVE_MCP_WAIT` configure it; `--check` prints the tool
count and the open project. Before the editor answers, `tools/list` is empty
and calls fail with a connection error, never with a hang. The shim needs only
Python 3.11.

An agent-facing walkthrough of the editing workflow lives in
`docs/skills/kdenlive-mcp-editing/SKILL.md`; copy or symlink that folder into
your agent's skills directory.

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
from `desktop_state`; bin IDs are numeric strings. Wherever a track is named
(`trackId`, `audioTrackId`, `trackIds`), its tag from `tracks[].tag` such as
`V1` or `A2` is accepted too, which survives a project reload while the numeric
ids do not.

| Tools | Parameters | Purpose |
| --- | --- | --- |
| `desktop_capabilities` | none | Read capabilities, including the state section names and the project's marker categories |
| `desktop_state` | optional `include` (section names), `trackId` (id or tag), `range` (`start`, `end`), `binIds` | Read the visible sequence, bin and history; see [State snapshot](#state-snapshot) |
| `desktop_frame_capture` | `position`, optional `width` (64 to 3840, default 540), `path` (a new `.png`) | Render one frame in a separate melt process, returned as a PNG image or written to `path`, without moving the playhead |
| `desktop_ui_capture` | `target` (`window`, `panel`, `page`), `panel` (dock name), `page` (`colors`, `type`, `spacing`, `icons`, `components`, `inventory`), optional `theme` (`dark`, `light`), `scale` (1 to 3), `width` (480 to 3840) | A PNG of the editor window, one open panel or one design review page, for [design review](design-review.md) |
| `desktop_design_audit` | optional `include` (`tokens`, `colors`, `contrast`, `type`, `icons`, `findings`, `inventory`), `panel` | The design tokens, contrast of every ink on every surface, type styles, icons, live findings and the panel list; see [design review](design-review.md) |
| `desktop_media_wait` | optional `binIds`, `timeoutSeconds` (0 to 120, default 30) | Wait until bin clips finish loading; returns `ready`, `timedOut` and each item's `duration` |
| `desktop_profile_list` | optional `query` | List project profiles (path, description, size, fps) and the current one |
| `desktop_render_cancel` | `path` | Stop a render started through MCP and remove its partial output |
| `desktop_effect_list` | optional `clipId` or `binId`, optional `query` | List a clip's effects with parameters and `keyframeOrigin`, and/or search the effect catalog |
| `desktop_title_read` | `binId` | Read a title clip's canvas, the project frame size and its items |
| `desktop_title_create` | `name`, `duration`, `items` (text: `text`, `x`, `y`, optional `fontPixelSize`, `fontFamily`, `bold`, `italic`, `color`, `opacity`, `alignment`, `boxWidth`, `letterSpacing`, `outline`, `shadow`; `kind: "rect"`: `x`, `y`, `width`, `height`, `color`, `opacity`, `borderColor`, `borderWidth`), optional `background` (`color`, `opacity`) | Create a native editable title, with exact duration in project frames and the project canvas size; see [Titles](#titles) |
| `desktop_project_new` | `profile` (path or description), `path` (new `.kdenlive`), optional `videoTracks`, `audioTracks` | Create and save a new project; ends the editing session; see [Projects and profiles](#projects-and-profiles) |
| `desktop_render_status` | none | Follow the progress of renders started by `desktop_render` |
| `desktop_render_verify` | `path`, optional `includeAudio` | Check a tracked video's dimensions, duration and complete decoding; return a WAV audio block for a short preview |
| `desktop_preview_render` | `path`, `start`, `end` (exclusive), optional `width` | Export at most 10 seconds of the visible sequence with audio, at reduced resolution |
| `desktop_batch_validate` | `commands` | Preflight normal insertions, including linked audio and collisions between proposed clips, without editing the project |
| `desktop_transcription_readiness` | optional `engine`, `model` | Check installed speech recognition prerequisites without starting recognition or installing packages |
| `desktop_marker_export` | `format` (`json`, `csv`, `kdenlive`), optional `binId` | Return the sequence guides, or a bin clip's markers, as text |
| `desktop_history` | optional `limit` (1 to 1000, default 50), `origin` (`all`, `mcp`, `user`, `unknown`), `sessionId`, `since` (undo index or ISO 8601 time), `includeUndone` (default true) | Read the shared Undo history as a change log; see [Change log](#change-log) |
| `desktop_media_import`, `desktop_media_remove` | `path`, optional `wait` (seconds); `binId` | Add files to the bin (optionally waiting until loaded) or remove unused assets |
| `desktop_clip_insert` | `binId`, `trackId`, `position`, `sourceIn`, `media` (`video`/`audio`), optional `sourceOut` (default: the whole clip), `clamp`, `mode` (`normal`, `overwrite`, `insert`), `linked`, `audioTrackId`, `allTracks` (insert mode) | Insert a bin clip range; an A/V clip as a linked pair by default; returns `clipId`, `audioClipId`, `groupId`; see [Timeline editing](#timeline-editing) |
| `desktop_clip_replace` | `clipId`, `binId`, optional `sourceIn`, `linked`, `keepEffects` | Swap one timeline clip (and its linked partner) for another bin clip in place, keeping position, group and effects; see [Composition](#composition) |
| `desktop_clip_fade` | `clipId`, `fadeIn` and/or `fadeOut` (frames, 0 removes), optional `linked` | Add Kdenlive's fade effects to a clip and its linked partner |
| `desktop_audio_duck` | `clipId`, `ranges` or `auto: true` (optional `trackIds`), optional `levelDb`, `duckDb`, `rampFrames`, `mute` | Keyframe a music clip's volume down under speech or other clips |
| `desktop_clip_remove` | `clipId`, optional `mode` (`lift`, `extract`), `group` (`whole`, `single`), `allTracks` (extract) | Remove a clip or its group, leaving a gap or rippling; returns `removedClipIds` |
| `desktop_clip_split` | `position` and one of `clipId`, `trackId` or `allTracks: true` | Cut clips at a frame; returns `pieces` (`trackId`, `leftClipId`, `rightClipId`) |
| `desktop_range_remove` | `start`, `end` (exclusive), `trackIds` or `allTracks: true`, optional `mode` (`lift`, `extract`) | Lift or extract a time range; returns `removedClipIds`, `newClipIds` |
| `desktop_gap_remove` | `position`, `trackId` or `allTracks: true` | Close the gap at a frame; returns `removed` (frames) |
| `desktop_space_insert` | `position`, `duration`, `trackId` or `allTracks: true` | Insert blank space, moving later clips right |
| `desktop_clip_group`, `desktop_clip_ungroup` | `clipIds` (2 to 200); `clipId` or `groupId` | Group clips (the halves of one A/V clip become linked); dissolve a topmost group |
| `desktop_clip_speed` | `clipId`, `speed` (factor, 0.01 to 100 in magnitude, negative reverses), optional `pitchCompensation` | Change playback speed with the linked clip; returns `speed`, `duration` |
| `desktop_clip_enable` | `clipId`, `enabled`, optional `linked` (default true) | Disable or enable a clip and its linked partner |
| `desktop_media_replace` | `binId`, `replacementBinId` | Replace a media asset while preserving timeline ranges (not titles: edit those) |
| `desktop_clip_move`, `desktop_clip_trim` | `clipId`, `trackId`, `position`; `clipId`, `duration`, `edge` | Move clips and trim their edges |
| `desktop_audio_envelope`, `desktop_track_rename` | `clipId`, `fadeIn`, `fadeOut`, `gainDb`; `trackId`, `name` | Add audio fades/gain and name tracks |
| `desktop_project_save` | none | Save the project to its file |
| `desktop_undo`, `desktop_redo` | optional `count` or `toIndex`; `desktop_undo` also `revertSession: true` | Move through shared native history; see [Change log](#change-log) |
| `desktop_project_save_as` | `path` | Save a copy or an unsaved project's first file and keep editing it |
| `desktop_project_profile` | `width` and `height` (even, 16 to 8192) and/or `fps` | Change the frame size, e.g. 1080x1920 vertical (immediate, not undoable), or convert a saved project to another frame rate (opens the converted copy) |
| `desktop_clip_reframe` | `clipId`, `mode` (`fill`/`fit`), optional `focusX`, `focusY`, `endFocusX`, `endFocusY`, `zoom` | Fill or fit a clip in the frame with a Transform effect, with focus point, pan and zoom |
| `desktop_effect_add`, `desktop_effect_set`, `desktop_effect_remove` | `clipId` or `binId`; `effectId` and optional `params`; `index` and `params`; `index` | Edit effects on timeline clips or bin clips with MLT parameter strings |
| `desktop_title_edit` | `binId`, optional `width`, `height`, `items` (`index`, `text`, `x`, `y`, `fontPixelSize`, `alignment`, `color`, `bold`, `fontFamily`) | Edit a native or imported title clip's canvas size and text items |
| `desktop_marker_add` | `position`, optional `duration`, `comment`, `category`, `binId` | Add a guide, or with `binId` a clip marker; replaces a marker at the same frame |
| `desktop_marker_edit` | `position`, optional `binId`, then any of `newPosition`, `duration`, `comment`, `category` | Change one guide or clip marker |
| `desktop_marker_remove` | optional `binId`, and `position`, `all: true`, or `category` and/or `range` (`start`, `end`) | Remove one or many markers in one Undo step |
| `desktop_marker_import` | `format`, `text`, optional `binId` | Add markers from exported text in one Undo step |
| `desktop_transcript` | one of `binId`, `clipId` or `range` (`start`, `end`; optional `trackIds`), optional `format` (`words`, `segments`, `text`, `srt`), `minPause` (seconds, default 0.5) | Read a clip's word-level transcript with source and timeline times, and the pauses in it; see [Transcripts and silence](#transcripts-and-silence) |
| `desktop_transcript_import` | `binId`, `format` (`json`, `srt`, `vtt`, `whisper`), `text`, optional `language`, `engine`, `append` | Store a transcript on a bin clip in one Undo step |
| `desktop_silence_detect` | one of `binId` (optional `sourceRange` in seconds), `clipId` or `range` (optional `trackIds`), optional `thresholdDb` (default -35), `minDuration` (s, default 0.5), `padding` (s, default 0.1) | Find silences and speech with ffmpeg `silencedetect` |
| `desktop_range_cut` | one of `ranges` (`start`, `end` frames), `words` (`from`, `to` indexes, with `binId` or `clipId`) or `silences` (`true` or `{thresholdDb, minDuration, padding}`, with `binId`, `clipId` or `range`); optional `keep`, `range`, `mode` (`extract`, `lift`), `trackIds` or `allTracks`, `padding`, `minGap` (frames), `addMarkers` (a category), `dryRun` | Remove many ranges in one Undo step; returns `removedRanges`, `removedFrames`, `newDuration`, `clipIdsAffected` |
| `desktop_transcribe`, `desktop_transcribe_status` | `binId`, optional `engine` (`whisper`, `vosk`), `model`, `language`; optional `jobId` | Run Kdenlive's speech to text on a clip and follow the job |
| `desktop_render` | `path`, optional `preset`, `range` (`start`, `end`) | Render the active sequence, or a range of it at full quality, with a preset; two renders may run at once |
| `desktop_batch` | `commands` (1 to 200 `desktop_apply` commands) | Apply edits as one Undo step, rolled back on the first failure |
| `desktop_apply` | `request` with the edit fields and one `command` | Submit any supported operation using the common request envelope |

Tool annotations follow the MCP hints. The reading tools are
`readOnlyHint`; `desktop_render_cancel` edits no project content and needs no
edit fields. `desktop_media_import`, `desktop_audio_envelope`,
`desktop_project_save_as`, `desktop_effect_add`, `desktop_clip_group` and
`desktop_render` only add content and are not `destructiveHint`; every other
editing tool is (`desktop_clip_insert` can overwrite). Repeating
`desktop_clip_insert`, `desktop_effect_add`, `desktop_effect_remove`,
`desktop_range_remove`, `desktop_gap_remove`, `desktop_space_insert`,
`desktop_transcript_import` (with `append`), `desktop_range_cut`,
`desktop_transcribe`, `desktop_render`, `desktop_undo`, `desktop_redo`,
`desktop_batch` or `desktop_apply` with new edit fields has a further effect, so
they are not `idempotentHint`. Independently of the hints, an identical retry
with the same `requestId` returns the saved result.

Read `desktop_state` before editing. Each edit needs its `sessionId`, the
`expectedRevision`, and a unique `requestId`; every successful edit returns the
new `state`, so a client can chain edits from the embedded `revision` without
reading state again. Frame values use the project FPS; `sourceOut` is exclusive.
Import and replacement load asynchronously: pass `wait` (seconds) to
`desktop_media_import`, or import many files and call `desktop_media_wait` once
(`binIds` narrows it; `desktop_state` with `binIds` is the cheap poll). A
clip that is still loading fails with `MEDIA_NOT_READY`; an unknown bin id with
`UNKNOWN_BIN`.

`desktop_clip_insert` takes `sourceOut` optionally: left out, the whole clip
is inserted. Generated audio stems are often a frame or two shorter than the
range they were cut for; `clamp: true` trims `sourceOut` to the media and the
result reports `clamped: true` with the `sourceOut` used, instead of
`INVALID_COMMAND` with `maxSourceOut`.

## Authoring and review

The first save of an unsaved project is allowed only inside the configured
additional media folder. Other edits continue to require a saved project.
Output paths cannot overwrite files or escape allowed folders through symlinks.
First save keeps the current project's frame rate and tracks; this API does not
create a separate project or change the frame rate.
`desktop_capabilities.project` reports whether the project is saved, whether
first save is available, and the configured `firstSaveFolder`.

`desktop_title_create` uses Kdenlive's native title creation, not an imported
template file. Text is plain text, XML characters are escaped, coordinates are
in project pixels, and `duration` is an exact frame count. Font settings default
to Sans Serif, 54 pixels, normal weight and white; `bold` maps to Qt's actual font
weight. Colors use `#RRGGBB`. Wait for the returned bin ID to become ready, then
insert `[0,duration)`. The title is readable with `desktop_title_read`, editable
with `desktop_title_edit`, and removable through native Undo. The read result
also includes `fontFamily` and `bold`.

`desktop_batch_validate` projects normal clip insertions over the current
timeline. It returns `valid`, `issues` with zero-based `commandIndex`, and the
session/revision it inspected. It checks the same source/stream/track constraints
as actual insertion, plus collisions on video and linked audio tracks, including
between proposed insertions. Invalid ranges report supplied values and
`maxSourceOut`. It never applies edits, advances the revision, changes selection,
or writes Undo history. Other operation types and ripple/overwrite modes produce
`VALIDATION_UNSUPPORTED`; they are not silently considered valid. Execution still
requires a fresh revision check because the user can edit after preflight.

Before choosing speech recognition, call `desktop_transcription_readiness`.
The result includes `ready`, `engine`, the selected `model`, `installedModels`,
`missingDependencies`, and a reason/hint when unavailable. It uses the same
environment checks as `desktop_transcribe`, but never installs packages,
downloads a model, or starts recognition.

For a still, `desktop_frame_capture` renders one frame of the sequence in a
separate melt process (decoding the sequence a second time inside the editor
competed with the monitor for MLT's producer cache and could abort the
application). The frame comes back as a PNG image block, or with `path` is
written to a new `.png` inside the allowed folders at the requested `width`
(up to the project width), with a resource link. It takes a second or two per
frame; capture a handful of checkpoints, not every cut.

For review, call `desktop_preview_render` with a new `.mp4` path and `[start,end)`
in timeline frames. A preview includes mixed timeline audio, lasts at most ten
seconds, and defaults to width 640. The even width can be 64 to 1280; the height
follows the project aspect ratio. Preview rendering keeps the playhead, project
profile, Undo history and media selection unchanged. Poll `desktop_render_status`,
then call `desktop_render_verify` with `includeAudio: true`. The result includes
verification metadata, an MCP WAV audio content block, and a resource link to
the playable video. The audio data is not repeated in structured JSON.

`desktop_render` exports the whole sequence or, with `range` (`start`,
`end`), only that part at the preset's full quality: a corrected end card or
one chapter without re-encoding the rest. Two renders may run at once
(`RENDER_BUSY` names the running ones beyond that). `desktop_render_status`
reports each job's `range`, `startedAt`, `progress`, `frame`, and while it
runs `framesPerSecond` and `etaSeconds`, so a slow section is not mistaken for
a stall. `desktop_render_cancel` stops a job through the renderer's own abort
(the renderer removes its partial output), or by stopping the process tree
when it has not connected yet, and reports it as `aborted`.

MCP render records are saved atomically in `mcp-renders.json` under Kdenlive's
application data directory. Completed jobs survive editor restarts. Jobs whose
completion notification was lost become `unconfirmed`, never automatically
`finished` just because a file exists. On Linux, a recovered job whose renderer
is no longer running is reported as `interrupted`. Verify its output to recover
a completed export, or render again to a new path if verification fails. A live
recovered renderer continues to block another render.
The journal retains the 100 most recent records. Render-start results include
`trackingPersisted`; a false value means the journal could not be written.

`desktop_render_verify` reads only tracked output files inside the allowed
folders. It checks video dimensions, expected duration and complete FFmpeg
decoding, with a 15-second probe timeout and a 60-second decoding timeout. An
incomplete MP4 or a shortened output fails verification. A timeout is reported
as a verification failure, not proof that the file is corrupt. Successful
verification confirms a recovered job as `finished`. This checks technical
integrity; playback review remains necessary to judge cuts and pacing.

## Projects and profiles

`desktop_profile_list` returns Kdenlive's profile catalog (`path`,
`description`, `width`, `height`, `fps`, `progressive`, `current`), filtered by
`query` (`"1080p 25"`). `desktop_project_new` takes one of those `path`s or
descriptions, a new `.kdenlive` `path` inside the allowed folders and optional
`videoTracks`/`audioTracks`, runs Kdenlive's File > New without its dialog and
saves the project there. The open project must be unmodified
(`PROJECT_MODIFIED`: save or undo first); a stale autosave of an untitled
project is reported as `BACKUP_PENDING` with the files, because Kdenlive would
otherwise ask about it in a dialog. The call ends the editing session: the
result has `sessionEnded: true`, the new `sessionId`, `revision`,
`documentUrl` and `ready` instead of a `state`; read `desktop_state` before
the next edit.

`desktop_project_profile` changes the frame size immediately (`width`,
`height`, not undoable). With `fps`, and a saved, unmodified project, it runs
Kdenlive's frame-rate conversion: positions, guides and lengths are rescaled
into a copy named after the rate (`fresh-2500.kdenlive` for 25 fps, reported
as `convertedFile`; `FILE_EXISTS` when that file is already there) and the
copy is opened, ending the session like `desktop_project_new`. Poll
`desktop_state` until `documentUrl` names the converted file.

## Titles

`desktop_title_create` writes Kdenlive's native title XML, so the result is
editable in the title editor and with `desktop_title_edit`. `items` are drawn
in order:

- text items: `text`, `x`, `y` (project pixels), `fontPixelSize` (default 54),
  `fontFamily`, `bold`, `italic`, `color` (`#RRGGBB`, default white),
  `opacity` (0 to 1), `alignment` (`left`, `center`, `right`; centred and
  right-aligned text aligns inside `boxWidth`, default the rest of the frame
  from `x`), `letterSpacing`, `outline` (`width`, `color`, `opacity`) and
  `shadow` (`color`, `opacity`, `blur`, `xOffset`, `yOffset`);
- `kind: "rect"` boxes: `x`, `y`, `width`, `height`, `color`, `opacity`,
  `borderColor`, `borderWidth`. Put a translucent rect before its text for a
  readable lower third.

`background` (`color`, `opacity`) fills the whole frame; the default is
transparent. `duration` is an exact frame count. Wait for the returned bin ID
to become ready, then insert it (`sourceOut` can be left out). Imported
`.kdenlivetitle` files (template titles) are read and edited the same way:
`desktop_title_read` reports `template: true` and the item `color`;
`desktop_title_edit` changes text, position, size, alignment, `color`,
`bold` and `fontFamily`. Titles have no media file, so
`desktop_media_replace` refuses them (`INCOMPATIBLE_MEDIA`): change the text
instead, or swap one timeline instance with `desktop_clip_replace`.

## Composition

`desktop_clip_replace` swaps one timeline clip for bin clip `binId` in one
Undo step (`Replace clip`): the new clip takes the same track, position and
duration, starts at `sourceIn` (default: the old clip's), keeps the former
group and, unless `keepEffects: false`, the user-added effects of the old clip
(built-in effects belong to each clip). The linked A/V partner is replaced too
when the replacement has that stream and `linked` is not false, keeping its
offset to the main clip; a replaced pair is linked again. The replacement must
be ready and cover the range (`MEDIA_TOO_SHORT` with `sourceIn`, `sourceOut`
and `replacementDuration`); nothing is removed when the insertion fails. The
result lists `replaced[]` (`clipId`, `newClipId`, `trackId`, `position`,
`duration`, `sourceIn`, `effects`), the new `clipId`, `linkedClipId`,
`groupId` and `effectsRestored`. This is the safe way to drop in a levelled
or re-recorded stem: never remove the old audio before the new one is in.

`desktop_clip_fade` adds Kdenlive's own fade effects (`fade_from_black` /
`fade_to_black` on video tracks, `fadein` / `fadeout` on audio tracks) over
`fadeIn` and/or `fadeOut` frames, on the clip and its linked partner unless
`linked: false`; `0` removes that fade. The fades must leave at least one
frame. One Undo step (`Fade clip`).

`desktop_audio_duck` keyframes the volume of a music or ambience clip on an
audio track: `levelDb` (default 0) outside and `duckDb` (default -18;
`mute: true` uses -100) inside the given timeline `ranges`, or with
`auto: true` under every clip on the other audio tracks (or `trackIds`).
Ranges are clipped to the clip, merged when their gap cannot hold two ramps,
and each edge ramps over `rampFrames` (default half a second). Keyframes are
written in the clip's source frames (speed applied), as `desktop_effect_list`
reports them, into the clip's existing volume effect or a new one
(`replacedVolume`). `NO_RANGES` when nothing overlaps the clip.

## Timeline editing

The timeline tools call the operations behind Kdenlive's own timeline actions
and push one Undo step each (`Cut clip`, `Extract zone`, `Insert space`, ...);
inside `desktop_batch` they share the batch's single step. Edits clear the GUI
selection first, so a selection never widens them, and they run in Kdenlive's
normal edit mode whatever the GUI's insert/overwrite toggle says. Locked tracks
are never changed: commands naming one fail with `TRACK_LOCKED`, and
`allTracks: true` means every unlocked track.

- Insert modes. `desktop_clip_insert` with `mode: "normal"` (default) refuses
  a range that is not empty on a receiving track (`OVERLAP`). `overwrite`
  lifts the range from the receiving tracks first, as Kdenlive's overwrite mode
  does. `insert` cuts clips at `position` and moves everything after it right by
  the inserted duration on the receiving tracks, or on every unlocked track
  with `allTracks: true`, as Kdenlive's insert does; other tracks stay put.
- Linked A/V. With `media: "video"`, a bin clip with audio and video is inserted
  as a pair by default: the video on `trackId`, the audio on `audioTrackId`, or
  else the track's mirror audio track or the nearest unlocked audio track. The
  halves are grouped and linked, as dropping the clip in the GUI does
  (`groupId`, `linkedClipId` in state). `linked: false` inserts the video only;
  without a usable audio track the default falls back to video only, while
  `linked: true` fails with `NO_AUDIO_TRACK`.
- Lift and extract. `lift` removes and leaves a gap (`tracks[].gaps`);
  `extract` closes it. `desktop_clip_remove` extracts each removed clip's range
  on that clip's own track, as the GUI's Extract does; with `allTracks: true`
  it removes the clip's time range from every unlocked track, like Extract zone.
  `desktop_range_remove` lifts or extracts `[start, end)` on `trackIds` or all
  unlocked tracks, cutting clips that cross either end. Compositions that
  overlap a lifted range are removed with it, as in the GUI.
- Groups. `desktop_clip_remove` removes the clip's whole group by default
  (`group: "whole"`, what the GUI deletes); `group: "single"` takes the clip out
  of its group first, which unlinks an A/V pair. Splits cut every grouped clip
  under the position and regroup the halves on each side, so a linked pair
  becomes two linked pairs. A grouped clip on a track outside a range or
  ripple is ungrouped from the part inside it (Kdenlive's
  `breakAffectedGroups`), and rippling moves only the listed tracks, so a pair
  split across listed and unlisted tracks can drift out of sync: list both
  tracks of linked pairs, or use `allTracks`.
- Gaps and space. `desktop_gap_remove` is the GUI's Remove space: the gap must be
  blank on every affected track (`NO_GAP` otherwise, also when no clip follows),
  and a gap between two clips of one group is refused. `desktop_space_insert`
  is Insert space: items at or after `position` (a clip spanning it moves whole)
  move right with their groups; `NOTHING_TO_MOVE` when there is none.
- Guides do not move with ripple edits while Kdenlive's "Lock guides" setting
  (`lockedGuides`, on by default) is on. With it off they follow as in the GUI:
  `desktop_gap_remove` moves the guides after the gap, `desktop_space_insert`
  and extracts move them when every track ripples (`allTracks: true` and no
  locked track; an extract also deletes guides inside the range). Results report
  `guidesMoved`.
- Speed. `desktop_clip_speed` takes a factor (the state's `speed`), changes the
  linked clip too and keeps the source range, so the duration scales;
  `OVERLAP` when a slower clip would run into the next one. Media without a
  fixed duration (colour, title) fails with `INCOMPATIBLE_MEDIA`.
- `NOT_SPLITTABLE`: the position is at a clip edge, outside it, or (for
  `trackId`/`allTracks`) no clip spans it. `UNKNOWN_GROUP`: the clip is not
  grouped, or `groupId` is not a topmost group from state.
- `desktop_clip_move` has no insert or overwrite mode: Kdenlive's insert-mode
  move uses internal fake-move operations that are not part of the model's
  public interface.

To cut a span, such as a silence, out of linked audio and video and close it,
use one `desktop_range_remove` with `mode: "extract"` and `allTracks: true`
(or the pair's `trackIds`). For many spans use `desktop_range_cut`, which takes
the list (or computes it from a transcript or from silences) and removes it in
one Undo step; a `desktop_batch` of `remove_range` commands ordered from the
last span to the first does the same.

## Transcripts and silence

A transcript belongs to a bin clip: words with `start` and `end` in seconds of
the clip's source media. It is stored in the project as the bin clip property
`kdenlive:mcp_transcript` (compact JSON: `version`, `language`, `engine`,
`format`, `words[{start, end, text, confidence?, segment?, interpolated?}]`),
together with the same words in Kdenlive's own `kdenlive:speech` property, the
HTML the **Text-based edit** panel saves after speech recognition, so the panel
shows an imported transcript. Both go through the bin's Edit Clip command, so an
import is one Undo step (`Import transcript`), undone with the rest of the
history and saved with the project. A transcript the panel recognised itself is
read from `kdenlive:speech` (`origin: "kdenlive:speech"`, `engine: "kdenlive"`,
one segment per paragraph); when the panel recognises again after an MCP import,
its newer result wins. Replacing the clip's media (`desktop_media_replace`)
reloads the clip and drops the transcript. Transcripts are not part of
`desktop_state`.

`desktop_transcript_import` formats:

- `json`: a words array or `{language, engine, words}`; each word has `start`,
  `end`, `text` (or `word`), optional `confidence` (0 to 1, or `probability`)
  and `segment` (index). Unknown keys are ignored, so `desktop_transcript`
  output can be imported again.
- `srt` and `vtt`: cue timing only. Each cue's text (tags removed) is split into
  words and its duration divided equally between them; such words carry
  `interpolated: true`. Each cue is a segment. WebVTT headers, `NOTE`, `STYLE`
  and `REGION` blocks and cue settings are skipped.
- `whisper`: openai-whisper JSON (`--output_format json`, `word_timestamps`):
  `language` and `segments[].words[]` (`word`, `start`, `end`,
  `probability`); a segment without words is interpolated like a cue.

An import replaces the clip's transcript; `append: true` adds the words to it.
Words are sorted by `start`; one lying more than a second past the clip's end
fails with `INVALID_TRANSCRIPT`, as does unparsable text. A tool call carries at
most about 60 KiB of text (the request limit), so import long transcripts in
parts with `append: true`. Clips without audio fail with `NO_AUDIO`, sequences
with `SEQUENCE_PROTECTED`.

`desktop_transcript` reads the transcript of a bin clip (`binId`: source times
only), of one timeline clip (`clipId`: the words its source window shows), or of
every clip in a timeline `range` on the unmuted audio tracks or `trackIds`
(a linked video clip whose audio half is covered is left out). Each word has
`index` (its position in the bin clip's word list), `text`, `start`/`end`
(source seconds), `sourceStart`/`sourceEnd` (source frames at the project frame
rate) and `confidence`, `segment` or `interpolated` when known. Through the
timeline, words add `clipId`, `trackId` and `timelineStart`/`timelineEnd`:

```
timeline frame = position + round(seconds × fps / speed) − sourceIn
```

where `sourceIn` and `speed` are the clip's state values (for a slowed clip,
`sourceIn` counts frames of the speed-adjusted source). Times are clamped to
the clip, and words partly outside it are `clipped`. Time-remap curves are
ignored and reversed clips are refused (`INCOMPATIBLE_MEDIA`) or skipped in a
range. `format` picks `words` (default), `segments` (`text`, times,
`firstWord`, `lastWord`; the stored segments when every word has one, otherwise
split after `.`, `?`, `!` and pauses longer than `minPause`), `text` (one
segment per line) or `srt` (cues in source time for a bin clip, timeline time
otherwise). Every format returns `gaps`: pauses longer than `minPause` seconds
(default 0.5) between words and at the edges of the clip's window, with
`afterWord`/`beforeWord` (-1 at an edge), so dead air shows up without another
call. `NO_TRANSCRIPT` when the clip (or every clip in the range) has none.

`desktop_silence_detect` runs `ffmpeg -af silencedetect` (the ffmpeg configured
in Kdenlive's environment settings) synchronously on the clip's audio stream,
only over the span it needs: the whole bin clip or `sourceRange` (seconds), the
source window of `clipId`, or the parts of the clips inside `range`. Silences
are quieter than `thresholdDb` for at least `minDuration` seconds; each is then
shrunk by `padding` seconds where it meets speech, keeping a little air around
words (`detectedStart`/`detectedEnd` keep the raw detection). `speech[]` is the
complement. For a `range`, `silences` are the timeline frames where none of the
covered clips has speech, so frames without any clip count as silent; per-clip
results are in `clips[]`, and a range without any audio clip on the measured
tracks fails with `NO_AUDIO`. At most two hours of audio per call (`TOO_LONG`);
ffmpeg failures and time-outs are `ANALYSIS_FAILED`, clips without audio
`NO_AUDIO`.

`desktop_range_cut` turns one of three selections into timeline ranges:
`ranges` in timeline frames; `words` (`{from, to}` inclusive indexes from
`desktop_transcript`) through `clipId` or through every instance of `binId` in
the active sequence; or `silences`, as `desktop_silence_detect` computes them on
`binId`, `clipId` or `range` (in a range, measured on the cut tracks' unmuted
audio tracks). `keep: true` inverts the selection inside the clip spans (or
`range`, or the whole sequence for `ranges`), keeping only the selected takes.
The ranges are merged, shrunk by `padding` frames at both ends and dropped below
`minGap` frames (default 1). `dryRun: true` stops there and returns them;
it still needs the edit fields so that it reflects the current revision, adds
no history, and like every successful call advances the revision (use the
returned `state.revision`). Otherwise every range is removed with Kdenlive's
lift or extract on `trackIds` or every unlocked track (the default), last range
first, and `addMarkers` adds a guide of that category at each cut: at the join
for `extract`, over the gap for `lift`. The whole pass is one Undo step
(`Remove 3 ranges`, `Lift range`), one `desktop_history` entry with `source`,
`ranges`, `removedFrames` and `removedRanges` in its summary, and one
`desktop_undo` restores clips and guides. Results: `removedRanges`
(`start`, `end`, `duration` in frames before the edit), `removedFrames`,
`newDuration`, `removedClipIds`, `newClipIds`, `clipIdsAffected` (removed, new and
moved clips) and `markersAdded`. Guides follow ripples as described in
[Timeline editing](#timeline-editing). `transcript_import` and `range_cut` run
inside `desktop_batch`.

`desktop_transcribe` starts Kdenlive's own speech to text on a file clip's audio,
with the scripts and arguments the Text-based edit panel uses (Whisper's
`whispertotext.py` or Vosk's `speechtotext.py`, run by the speech-to-text
Python environment), and returns a `jobId`; one job runs at a time
(`TRANSCRIPTION_BUSY`). `engine` and `model` default to the ones configured in
Kdenlive. Poll `desktop_transcribe_status` for `state` (`running`, `finished`,
`failed`), `progress`, `wordCount` and `error` (an unknown `jobId` is
`UNKNOWN_JOB`; a script that cannot start is `TRANSCRIPTION_FAILED`, and one
that fails later leaves a `TRANSCRIPTION_FAILED` error on its job). When the job finishes, its words
(one segment per Whisper segment or Vosk result, Vosk confidences kept) are
stored as an import would, as one Undo step attributed to the `desktop_transcribe`
request in the change log; a job that hears no speech stores nothing. If the
Python environment, its packages or a model is missing, the call fails with
`TRANSCRIPTION_UNAVAILABLE` and `reason` (`python_environment_missing`,
`dependencies_missing`, `model_missing` or `script_missing`), `engine`,
`model`, `installedModels`, `missingDependencies` and a `hint`. MCP never
installs packages or downloads models: the user does that in **Settings →
Configure Kdenlive → Speech to Text**, by installing Whisper (or Vosk) and
downloading a model there. Clips without a local media file fail with
`INCOMPATIBLE_MEDIA`.

Examples, each one Undo step:

- Remove silences: `desktop_range_cut {clipId, silences: true, dryRun: true}`
  to review the ranges, then the same call without `dryRun` (add
  `addMarkers: 3`, a marker category, to see the cuts). For a whole multi-clip sequence, use
  `silences: {thresholdDb: -40, minDuration: 0.8}` with `range: {start: 0, end:
  <duration>}`.
- Cut these sentences: read `desktop_transcript {clipId, format: "segments"}`,
  pick the segments to drop and pass their word spans:
  `desktop_range_cut {clipId, words: [{from: 12, to: 19}, {from: 40, to: 44}]}`.
- Keep best takes: read the segments of the clip holding every take, choose
  the best take of each line and pass only those with `keep: true`:
  `desktop_range_cut {clipId, words: [{from: 0, to: 8}, {from: 31, to: 39}],
  keep: true}` removes the rest of the clip and closes the gaps.

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

## Change log

Every MCP edit lands on Kdenlive's shared Undo stack, next to the user's own
edits. The editor watches that stack and records where each entry came from:

- `origin: "mcp"`: pushed while an MCP edit request ran. The entry carries
  `sessionId` (the editing session), `requestId`, `command` (the engine command
  type, such as `move`, `marker_add` or `batch`), `tool` (such as
  `desktop_clip_move`), `client` (a 12-character digest that identifies one MCP
  connection without revealing its session id) and `clientName` (its
  `clientInfo.name`), `revisionBefore` and `revisionAfter`, and a `summary` of
  the ids and positions the command named or returned (`clipId`, `binId`,
  `trackId`, `position`, `marker`, `effectIndex`...). A `desktop_batch` entry
  has `summary.commands` and `children[]` (`type`, `summary`) from the batch
  plan and its results.
- `origin: "user"`: any other entry, such as a GUI edit or an asynchronous
  change; it carries only the common fields.
- `origin: "unknown"`: entries already on the stack when the editor first saw
  the project, or several entries that appeared at once.

Every entry has `undoIndex` (the Undo index once it is applied, so an edit's
entry has the `undo.index` its result reports), `text` (without Kdenlive's
`hh:mm` prefix), `rawText`, `timestamp` (ISO 8601, UTC; when the editor first
saw it) and `state`: `applied`, or `undone` when its `undoIndex` is above the
current Undo index. A new edit after Undo discards the undone entries, as the
stack does. The record lives as long as the project stays open; another
document starts a new one, and switching sequences keeps it.

`desktop_history` returns `entries[]` oldest first (the newest `limit`
matching ones), `matched`, `undo` (`index`, `count`), `counts` (`applied`,
`undone`, `mcp`, `user`, `unknown` over the whole stack) and `logFile`.
`since` takes an undo index, returning later entries only, or an ISO 8601
time, returning entries seen at or after it. Bad filters fail with
`INVALID_ARGUMENTS`. `desktop_state` `include: ["history"]` embeds the last 10
entries.

To revert, `desktop_undo` and `desktop_redo` take `count` (steps) or `toIndex`
(undo down to, or redo up to, that Undo index; `undoIndex - 1` undoes back to
just before an entry). `desktop_undo` with `revertSession: true` undoes every
edit the calling MCP connection made in the current editing session, back to
the oldest one still applied, but only when every entry above it is also
theirs. Otherwise it fails with `HISTORY_INTERLEAVED`, lists the entries in the
way as `blocking` and returns the `toIndex` a full revert would need, so the
user can decide. Results list the `undone` or `redone` entries with `steps`
and the final `undoIndex`. Asking for more steps than exist, or a revert with
nothing to revert, fails with `EMPTY_HISTORY`; `toIndex` in the wrong direction
fails with `INVALID_ARGUMENTS`. Reverting moves the shared stack like the Undo
button; nothing is rewritten. Kdenlive merges repeated changes of one effect
parameter made within a few seconds into one entry, which keeps the origin of
the first change.

**Log file.** With **Write a change log of MCP edits** on (the default, in
the MCP API settings, `mcpWriteLog`), each MCP edit appends one JSON line to
`.kdenlive-mcp-log.jsonl` in the project's folder, or to `mcp-log.jsonl` in
Kdenlive's data directory (`~/.local/share/kdenlive`) when that folder is not
writable. Untitled projects are never logged. An `edit` line holds
`documentUrl`, `sessionId`, `requestId`, `command`, `tool`, `client`,
`clientName`, `undoIndex`, `text`, `timestamp`, `revisionBefore`,
`revisionAfter`, `summary` and, for batches, `children`. MCP undo, redo, save,
save-as, profile changes and renders, which add no entry, are logged as events
of those names (`undo` and `redo` lines list the moved `entries`). The file is
only appended to; delete it to clear it.

## State snapshot

`desktop_state` and every successful edit return the same snapshot shape.
Edit results embed it as `state`, always with the default sections. Frames are
project frames; ranges end exclusively.

These fields are always present: `sessionId`, `revision`, `sequenceId` (the
active sequence UUID), `documentUrl`, `modified`, `fps` (`numerator`,
`denominator`), `profile` (`width`, `height`, `description`, `duration`),
`playhead`, `activeTrackId`, `undo` (`index`, `canUndo`, `canRedo`,
`undoText`, `redoText`), `lastChange` (the newest MCP-made history entry still
on the stack: `undoIndex`, `command`, `text`, `requestId`, `applied`; null when
there is none), `sections` (the sections returned) and `counts`
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
| `history` | no | `history[]`: the last 10 history entries, as `desktop_history` returns them |

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

Save-as, project-new, render and frame-capture outputs follow the same folder
policy as imports, and none overwrites an existing file. Effect keyframes count frames from the
`keyframeOrigin` that `desktop_effect_list` reports. A profile change rebuilds
bin producers: filters that Kdenlive does not list as effects are dropped, so
save a copy first.

This version does not expose track creation, downloads or the companion's
offline project tools; `desktop_project_new` creates projects. Native state
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

**Acceptance checks.** The run calls all 60 tools. It checks the catalog against
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
marker error codes. It then exercises the timeline primitives on the fixture:
splitting the linked pair into two linked pairs and undoing it, splitting all
tracks, lifting (gaps) and extracting (later clips move) a grouped clip whole,
single and across all tracks, a split and a range extract in one batch Undo
step, range lift and extract, closing gaps and inserting space, linked, insert-mode,
all-track insert-mode, overwrite and audio-only insertion, `OVERLAP`, grouping and
ungrouping with A/V linking and nesting, speed changes with undo and `OVERLAP`,
enabling and disabling with and without the linked clip, the history summaries
of these edits, and their refusals on the locked track; one `toIndex` undo then
restores the fixture layout. Transcripts and silence come next: a generated
20 s tone with two silences goes on A1, and silence detection on the bin clip,
the timeline clip and a range must find them within a frame, padded and mapped
to timeline frames; the fixture's Text-based edit transcript is read back;
json, srt, vtt and whisper imports are checked word by word, and through the
pair's clips (source in 10 s, speed 0.5) in timeline frames, as words,
segments, text, srt and gaps; `desktop_range_cut` removes words, keeps takes,
lifts and extracts ranges, and cuts every silence with guides at the joins in
one Undo step that one `desktop_undo` restores, with its history summary; an
import and a cut share one batch step; the error codes are checked; and
`desktop_transcribe` must report `TRANSCRIPTION_UNAVAILABLE` with its reason
when no speech-to-text environment is installed (with one, it waits for the
job). It then tests inserting, moving,
trimming and removing clips with undo/redo; receipt replay, `REVISION_CONFLICT` and
`REQUEST_ID_REUSED`; the import folder policy, including a symlink escape;
importing, removing and replacing media (which drops a transcript); the audio
envelope and track rename; saving and reopening, which yields a new session and
keeps the transcript imported before saving. After the restart it checks
the change log: `lastChange`, entry origins, commands, tools, summaries and
batch children, applied and undone states through `count` and `toIndex`
undo/redo, the history filters and their errors, the `history` state section,
`revertSession` refused with `HISTORY_INTERLEAVED` while a second MCP
connection's edit sits in between (the offscreen editor has no GUI input, so
that connection stands in for another editor) and succeeding once it is gone,
and the lines of both editor sessions in `.kdenlive-mcp-log.jsonl`. It then captures frames and
checks the PNG image block and its size; searches the effect catalog; adds,
sets, removes and undoes an effect and reads it in the `effects` section, with
unknown effects, indexes and parameters rejected; reads and edits a title clip from the fixture, with undo/redo; runs
`desktop_apply`; applies a batch as one Undo step and checks that a failing batch
leaves the timeline, bin and history unchanged; saves a copy, with a path outside
the allowed folders refused; switches to 1080x1920 at the same frame rate; pans
and fits a clip with `desktop_clip_reframe`; resizes the title canvas; and
renders the 1080x1920 sequence, polling `desktop_render_status` until the file
is finished, with bad paths and an unknown preset refused, a second ranged
render started alongside and cancelled, a third refused, and a verified
range render. It also covers track tags, `clamp` and the default `sourceOut`,
`import` with `wait`, `desktop_media_wait`, the `binIds` filter, styled titles
and an imported `.kdenlivetitle`, `desktop_clip_replace`, `desktop_clip_fade`,
`desktop_audio_duck`, a saved frame capture, `desktop_profile_list`,
`desktop_project_new` and a frame-rate conversion. It takes about a minute.

Codex's HTTP configuration fields are documented in its
[configuration reference](https://developers.openai.com/codex/config-reference).
