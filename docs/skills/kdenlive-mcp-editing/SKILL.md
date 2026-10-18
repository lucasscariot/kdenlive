---
name: kdenlive-mcp-editing
description: Build, review and export a video (vlog, short, chapter edit) in a running Kdenlive through its native MCP tools (desktop_*). Use whenever an agent must assemble clips, titles, music and fades in Kdenlive, fix a cut, re-level audio, or render and verify an export. Covers the order of operations, the tools that avoid the classic failures (removing before inserting, moving files under a render, polling state in loops, hand-rolled HTTP clients), and how to review without touching the user's playhead.
---

# Editing in Kdenlive through MCP

The editor is the source of truth. Every tool reads or edits the project the
user sees, shares its Undo history, and returns the new `state` after each
edit. Work in passes: ingest, plan, assemble in one batch, dress (titles,
music, fades), review, export, verify.

## Before touching the timeline

1. **Connect through the harness, not a script.** The tools are named
   `desktop_*`. If they are missing from your tool list, the editor was not
   running when your session started: ask the user to start Kdenlive Live, or
   use the stdio shim (`packaging/live-mcp/kdenlive-mcp-stdio.py`, see
   `docs/native-mcp.md`) which launches it and proxies the HTTP endpoint.
   Writing your own HTTP client costs an escalation prompt per call.
2. `desktop_capabilities`, then `desktop_state`. Note `sessionId`, `revision`,
   the track tags (`V1`, `A1`...), `profile` and `fps`.
3. **Project and profile first.** A new project at the wrong frame rate is the
   most expensive mistake. `desktop_profile_list {query: "1080p 25"}` then
   `desktop_project_new {profile, path, videoTracks, audioTracks}`; the result
   ends the session, so read `desktop_state` again. For an existing project,
   `desktop_project_profile {width, height}` changes the size and
   `{fps}` converts a saved project (it opens the converted copy).
4. Save early: `desktop_project_save_as` inside the project folder or the
   configured additional media folder. Imports, renders and captures are only
   allowed there.

## Ingest

- Import everything with `desktop_batch` of `import` commands, then one
  `desktop_media_wait`. Do not poll `desktop_state` in a loop; if you must poll,
  pass `binIds` so the answer stays small. A single file can use
  `desktop_media_import {path, wait: 30}`.
- Reimporting a path returns the existing `binId` (`existing: true`), so a
  retry never duplicates assets.
- Speech: `desktop_transcription_readiness`, then `desktop_transcribe` and
  `desktop_transcript` (words with timeline frames), or
  `desktop_transcript_import` when you transcribed elsewhere.
  `desktop_silence_detect` finds dead air. Use them before writing your own
  transcription pipeline.

## Plan, then assemble in one step

- Write the cut as a list of `insert` commands: `binId`, `trackId` (tag `V1`
  is fine), `position`, `sourceIn`, optional `sourceOut` (whole clip when left
  out), `media: "video"` with `linked: true` for camera clips so the audio
  lands on `audioTrackId` and stays grouped.
- `desktop_batch_validate` with the whole list, fix the `issues`, then
  `desktop_batch`. The batch is one Undo step and rolls back entirely on the
  first failure.
- Speed changes (`desktop_clip_speed`), guides (`desktop_marker_add` or a
  `marker_import` of your chapter list) and track names can go in a second
  batch.
- Audio stems you generated (levelled dialogue, music beds) are often one or
  two frames shorter than the range you cut them for. Insert them with
  `clamp: true`, or better, **replace instead of remove-then-insert**:
  `desktop_clip_replace {clipId, binId, sourceIn}` swaps a timeline clip for
  the stem in place, keeps its group and effects, and leaves the old clip in
  place if the new one does not fit (`MEDIA_TOO_SHORT`).

## Dress the cut

- **Titles**: `desktop_title_create` makes a native, editable title. For a
  readable lower third put a translucent `kind: "rect"` before the text and
  give the text an `outline` and a soft `shadow`; use `alignment: "center"`
  with `boxWidth` for centred cards; `background` for a full card. Wait for
  readiness (`desktop_media_wait`), insert it on the title track with no
  `sourceOut`, and change wording later with `desktop_title_edit` (works on
  imported `.kdenlivetitle` files too). Never replace a title with
  `desktop_media_replace`.
- **Music under speech**: insert the music clip on its own audio track, then
  `desktop_audio_duck {clipId, auto: true}` to duck it under every clip on the
  other audio tracks, or pass explicit `ranges` (for example the speech
  ranges from `desktop_transcript`), `mute: true` for a passage that must stay
  silent (news excerpt, interview), `rampFrames` for the slope.
- **Fades**: `desktop_clip_fade {clipId, fadeIn, fadeOut}` on the first and
  last clips (video and audio halves together); `desktop_audio_envelope` for
  a simple gain plus edge fades on a stem.
- **Format changes** (vertical cut): `desktop_project_save_as` a copy,
  `desktop_project_profile {width, height}`, `desktop_clip_reframe` each shot,
  `desktop_title_edit {width, height}` for titles, then capture frames.

## Review without disturbing the user

- `desktop_frame_capture {position, width}` renders a frame in a separate
  melt process and returns a PNG; with `path` it writes the file. Capture a
  handful of checkpoints (opening, each chapter, every title, the credit).
  Each capture takes a second or two: do not capture every cut.
- `desktop_preview_render {path, start, end, width}` for up to ten seconds
  with audio, then `desktop_render_verify {path, includeAudio: true}` to hear
  the mix.
- `desktop_history` shows what you (and the user) changed; `desktop_undo
  {revertSession: true}` reverts only your own edits when they are not
  interleaved with the user's.

## Export and verify

- `desktop_render {path, preset}` for the whole sequence; `{range}` for a
  chapter or a corrected end card at full quality. Two renders can run at
  once; `desktop_render_cancel {path}` stops one and removes its partial file.
- Poll `desktop_render_status` every 20 to 40 seconds and read
  `framesPerSecond` and `etaSeconds`: accelerated or effect-heavy sections
  encode slowly and are not stalls.
- `desktop_render_verify {path}` decodes the whole file and checks size,
  duration and frame count. Treat it as a technical check, not an editorial
  one: watch the checkpoints you captured.
- A soundtrack or credit change after a long export does not need a full
  re-render: render the changed range, render the audio with the `WAV`
  preset, and assemble outside; or simply render again if time allows.

## Rules that prevent the known failures

- Never move, rename or delete a file that a project or a running render
  references. Clean up drafts after the render finished and only after
  checking `desktop_state` bin urls (absolute paths), never raw project XML.
- Validate before you destroy: `desktop_batch_validate` for insertions,
  `desktop_clip_replace` instead of remove + insert, `dryRun: true` on
  `desktop_range_cut`.
- Prefer tags (`V1`, `A2`) over numeric track ids when you persist a plan:
  numeric ids change when the project is reopened.
- Chain edits from the `state.revision` each edit returns; re-read
  `desktop_state` only after the user may have edited, or after
  `REVISION_CONFLICT`.
- Ask the user up front for language of titles, tone, music choice and
  length; redoing titles and credits is cheap through `desktop_title_edit`,
  but a wrong frame rate or aspect is not.
- Keep secrets out of logs: never print the MCP token or configuration files
  that contain it.
