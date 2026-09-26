# CI workflow templates

These are the intended GitHub Actions workflows for Pocket-Pirate-CYD.

The agent OAuth token that landed v0.2.0 **lacks the `workflow` scope**, so `.github/workflows/*` could not be pushed. To enable CI + tagged firmware releases:

1. Copy both YAML files into `.github/workflows/` on `main` (or open a PR with that change) using an account/token that has the `workflow` scope.
2. Push a tag `v*` (or use **Release firmware → workflow_dispatch**) to publish `.bin` assets automatically.

Until then, use the [v0.2.0 release binary](https://github.com/Evil0ctopus/Pocket-Pirate-CYD/releases/tag/v0.2.0) or `pio run -e cheap-black-display`.
