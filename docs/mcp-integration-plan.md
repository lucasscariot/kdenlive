# Built-in MCP server work plan

User objective: enable an API and port in the Kdenlive fork, then connect an
MCP client directly without running the companion TypeScript server.

- [x] Ground: trace the live editing engine, application lifecycle, and settings.
- [x] Sketch: compare native MCP transport and settings designs.
- [x] Agree: select the design autonomously; no approval checkpoint requested.
- [x] Implement: protocol, settings, native tools, acceptance checks, installation.
- [x] Scrap review: retain one editing engine and a thin HTTP transport.

Design comparison stages: frame, independent candidates, cross-judge, pick,
graft, verify. The rubric is a direct SDK connection, one native editing engine,
safe localhost lifecycle, discoverable settings, and tests against real Kdenlive.

The separate MCP implementation and current native bridge are checkpointed
before moving the transport into this repository. Existing editing operations
must remain revision-checked, undoable, and visible in the GUI.

## Selected design

Enable MCP in Settings → Configure Kdenlive → MCP API, choose a localhost port, and copy the Codex
configuration. The settings page reports whether the listener actually started.
The installed application owns the server and its token; no Node process is
needed for the live API.

Three independent candidates converged on Qt HTTP Server on the GUI thread,
one persistent editing engine, and bounded MCP sessions separate from editor
sessions. Candidate B is the base: it has the smallest request path and preserves
the dedicated media and timeline tools. Independent review scored it 25/25;
A and C each scored 24/25. The candidates used the available parent model;
the cross-review used a different available model family.

Adapt C's explicit Copy Codex Configuration action, separate private credential
file, and teardown before model destruction. Adapt A's explicit protocol-version
pinning and deployment checks. Reject custom HTTP parsing, an embedded JavaScript
runtime, and worker-thread editing queues. They add maintenance or ownership
problems without advancing the user's direct-connection objective.

## Boundaries and contracts

- `LiveBridge` owns editing, revisions, receipts and native Undo. Its timer must
  run even when D-Bus registration is disabled or fails.
- `McpTools` owns discovery and normalization into the existing apply envelope.
  It checks imported/replacement paths against the open project's directory and
  a user-configured additional media folder. Opening a saved local project in the
  GUI authorizes that project's edits. This replaces the companion's root flags.
- `McpServer` owns HTTP, authentication, MCP sessions, configuration and status.
  Qt parses HTTP with explicit body/header limits. The listener binds only to
  `127.0.0.1`, requires a private bearer token, and rejects browser origins.
- Core owns the engine and server. Changing ports retires connections and MCP
  sessions, while preserving editor receipts. Disabling stops the listener.
- Settings use the existing KConfig dialog. Saved enablement is distinct from
  actual running status and from project readiness.

The local SDK's latest supported protocol is `2025-11-25`; implement that
lifecycle with JSON responses, empty 202 notification acknowledgments, and
GET 405. Support its 2025-06-18 and 2025-03-26 compatible versions explicitly.
This is not a claim to implement every newer MCP revision.

The optional feature requires Qt 6.11 HttpServer and matching WebSockets runtime
libraries. Builds without the feature remain possible. Test an actual SDK against
the installed editor, including asset operations, Undo, duplicate requests,
save/reopen, auth, malformed requests, path containment, port changes and shutdown.

Sources: [MCP transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports),
[MCP lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle),
[Qt HTTP limits](https://doc.qt.io/qt-6.11/qhttpserverconfiguration.html).

## Verification, 2026-10-06

- Full editor build and user installation passed with Qt 6.11.2.
- Protocol tests passed with Kdenlive's strict Qt definitions: authentication,
  browser-origin rejection, lifecycle, malformed/oversized requests, port
  conflicts and changes, token rotation, disabling, and session invalidation.
- The MCP SDK connected directly to the native editor and made 60 tool calls.
  The test inserted, moved, trimmed and removed clips; imported, removed and
  replaced media; checked native Undo/Redo, fades, track naming, retry receipts,
  stale revisions, source-file retention, path/symlink rejection, and save/reopen.
- Applying the folder picker exposed a `file:` URL/path mismatch. The listener
  now normalizes local file URLs before configuration comparison and validation.
  A regression test preserves the session when the same folder is reapplied as
  a URL; the SDK acceptance also imports from a URL-configured folder with spaces.
- The acceptance editor had the legacy D-Bus editing bridge disabled. The Linux
  application build itself still uses the ordinary desktop session bus.
- Feature-off and API-on/D-Bus-off CMake configurations passed. These alternative
  configurations were not separately compiled in full.
- The installed copy resolved both optional Qt libraries from its private
  installation. A direct HTTP client verified the user's reopened film against
  its pre-upgrade snapshot. Timeline content and effects matched, with no unsaved
  changes. The settings page displayed the enabled endpoint and configured port.
- Codex parsed the new connection as `streamable_http` with authorization.
  Existing client sessions must reconnect to replace their cached tool catalog.

The review found one ownership correction: path policy must run after receipt
lookup, so retrying an already applied replacement still succeeds after its
temporary source bin item has been removed. `LiveBridge::applyAuthorized` keeps
that ordering inside the single editing engine. The HTTP layer contains no
timeline mutation logic. The SDK is a test dependency only.
