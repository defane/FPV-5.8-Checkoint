# FPV 5.8 Checkpoint

Minimal project skeleton with a devcontainer carrying AI coding agents and the
GitHub CLI:

- **Claude Code** (`claude`)
- **opencode** (`opencode`)
- **GitHub CLI** (`gh`)

## Getting started

1. Open this folder in the container:
   - VS Code: **Dev Containers: Reopen in Container**
   - CLI: `devcontainer up --workspace-folder .`
2. On create, the container installs `claude` and `opencode`
   (`postCreateCommand`) and adds `gh` (devcontainer feature).
3. The host gh token is bind mounted, so `gh` is already authenticated:
   - `~/.config/gh/hosts.yml` → `/home/node/.config/gh/hosts.yml`

   This file must exist on the host — run `gh auth login` there first. Otherwise
   remove the mount in `.devcontainer/devcontainer.json` and run `gh auth login`
   inside the container.
4. Authenticate the agents on first run:
   - `claude` — follow the login prompt
   - `opencode auth login`

That's all this skeleton provides — add your stack on top.
