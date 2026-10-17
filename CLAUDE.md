# Kdenlive PRO

Fork of KDE's Kdenlive. Upstream is https://invent.kde.org/multimedia/kdenlive (remote `upstream`, branch `master`).

- `kde-master` mirrors upstream `master` exactly. Never put fork work on it.
- `main` is `kde-master` plus every merged feature: build it, branch from it, open pull requests against it.
- One `feat/*`, `fix/*` or `docs/*` branch per change, with Conventional Commit titles.
- For anything about branches, syncing with KDE, or where a change belongs, use the `branch-keeper` agent (`.claude/agents/branch-keeper.md`).
- Feature documentation lives in `docs/`. The user launcher is `packaging/live-mcp/install-user.sh`, which installs to `~/.local/opt/kdenlive-live`; those directory and desktop-file identifiers keep the `kdenlive-live` name on purpose.
