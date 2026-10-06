# Native MCP API

The editor serves MCP directly over Streamable HTTP. No companion server,
Node runtime or D-Bus bridge is required for live editing.

## Enable and connect

1. Open **Settings → Configure Kdenlive → MCP API**.
2. Enable the local API, choose a port, and click Apply. The default is 8765.
3. Check that the status says `Listening on http://127.0.0.1:8765/mcp`.
4. Click **Copy Codex configuration**, add it to your Codex configuration, and
   reconnect the client. It includes the private authorization header.
5. Open and save a project in Kdenlive. MCP edits act on its visible sequence.

Import files from the project's folder or set an additional media folder here.
The API resolves symlinks before checking these folders. Existing project media
stays usable. Removing a bin asset never deletes the original file.

The server binds only to IPv4 localhost. It rejects browser origins and requires
a random bearer token stored in the application's config directory as `mcp-token`
with owner-only permissions. Regenerating the token disconnects clients; copy the
new configuration afterwards. Changing ports or disabling the API also ends MCP
connections. A busy port is reported in settings rather than silently replaced.

## Available tools

| Tools | Purpose |
| --- | --- |
| `desktop_capabilities`, `desktop_state` | Read capabilities and the visible project |
| `desktop_media_import`, `desktop_media_remove` | Add files to the bin or remove unused assets |
| `desktop_clip_insert`, `desktop_clip_remove` | Add or remove timeline clips |
| `desktop_media_replace` | Replace an asset while preserving timeline ranges |
| `desktop_clip_move`, `desktop_clip_trim` | Move clips and trim their edges |
| `desktop_audio_envelope`, `desktop_track_rename` | Add audio fades/gain and name tracks |
| `desktop_project_save`, `desktop_undo`, `desktop_redo` | Save and use shared native history |
| `desktop_apply` | Submit any supported operation using the common request envelope |

Read `desktop_state` before editing. Each edit needs its `sessionId`, the
`expectedRevision`, and a unique `requestId`. Frame values use the project FPS;
`sourceOut` is exclusive. Import and replacement load asynchronously. Poll state
until the affected bin asset is ready before using it.

Retry an uncertain edit only with its identical request ID and arguments. The
engine keeps 64 receipts across HTTP reconnections and port changes. Opening a
different document/sequence or restarting invalidates the editing session.
Manual changes advance the revision and share Undo with MCP edits.

The server implements the 2025-11-25 MCP lifecycle, with compatible 2025-06-18
and 2025-03-26 negotiation. It returns JSON responses, accepts notifications
with HTTP 202, and returns HTTP 405 for GET. It supports 32 sessions with a
30-minute idle expiry and enforces HTTP body/header limits through Qt.

This version covers the native live operations above. It does not yet expose
project creation, track creation, title authoring, arbitrary effects, rendering,
downloads or the companion's offline project tools. Open/create projects in the
GUI. Native state describes the active sequence and project bin.

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
request limits, token rotation, port conflicts, reconfiguration and shutdown.

The SDK acceptance test in `tests/mcp/acceptance.mjs` launches a disposable editor
and exercises native operations through HTTP. Node and the MCP SDK are test-only
dependencies. See its header for invocation. It never edits your open project.

Codex's HTTP configuration fields are documented in its
[configuration reference](https://developers.openai.com/codex/config-reference).
