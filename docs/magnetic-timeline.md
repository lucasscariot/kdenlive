# Magnetic timeline

Kdenlive Live can edit like Final Cut Pro: a primary storyline that never keeps a gap, clips
connected to it, roles instead of numbered tracks, and an index listing clips and roles.

## Magnetic mode

The fourth edit mode, next to Normal, Overwrite and Insert (Tool menu or the mode menu of the
timeline toolbar). The chosen mode is remembered (`timelineEditMode`) and applies to every
sequence.

- **Primary storyline**: the lowest video track, or the track chosen with *Use as Primary
  Storyline* in the track menu or the index. Its clips and their linked audio form the storyline,
  drawn on a darker lane.
- **Connected clips**: every other clip hangs from the storyline clip under its first frame. A clip
  over a gap hangs from the next storyline clip, a clip past the end keeps its distance to the end.
- **Edits**: deleting a storyline clip closes its gap and deletes the clips connected to it.
  Dragging a storyline clip reorders the storyline at the closest edit point; dragging it to
  another lane lifts it out. Dragging a clip onto the storyline inserts it. Trimming a storyline
  clip ripples the clips after it. Dropping or inserting a clip on the storyline pushes the clips
  after it. In every case the connected clips follow the clip they hang from, moving to another
  lane of the same role when theirs is taken, or to a new lane.
- Each edit is one undo step. The MCP bridge keeps editing in normal mode.

Connections are derived from the layout, so there is nothing to store: they survive saving,
loading and undo. The engine is `src/timeline2/model/magnetictimeline.{hpp,cpp}`: each edit takes
the connections, edits the storyline tracks with the normal mode primitives, then puts the
connected clips back at their offset. `tests/magnetictest.cpp` covers it.

## Roles

Each track has a role: Video or Titles for video tracks, Dialogue, Music or Effects for audio
tracks. The role is guessed from the track name until one is assigned (`kdenlive:track_role`,
saved with the track). Clips take the color of their role from the `role-*` design tokens,
unless a clip color was chosen in the settings.

## Timeline index and compact headers

*Timeline Index* (Ctrl+Shift+2, first button of the timeline toolbar) opens a panel left of the
timeline:

- **Clips**: the timeline clips in order, storyline clips first and connected clips indented, with
  a search field. A click selects the clip and moves the playhead to it.
- **Roles**: each role with a checkbox to show or mute all its lanes and a solo button, then its
  lanes with hide or mute, lock, rename (double click) and a menu to change the role or the
  storyline.

*Compact Track Headers* (Timeline menu, on by default) replaces the V1/A1 headers with a strip in
the role color. Clicking the strip toggles the insert target; a hidden, muted or locked track
shows a button to restore it; the right click menu holds the other track actions.
