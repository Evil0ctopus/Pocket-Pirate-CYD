# CI workflow templates

These YAML files are the intended GitHub Actions workflows for Pocket-Pirate-CYD.
Copies also live under `.github/workflows/` when the pushing token has the
`workflow` OAuth scope.

## If `.github/workflows/` push is rejected

Agent / fine-grained tokens sometimes lack the **`workflow`** scope. Symptoms:

- `refusing to allow a Personal Access Token to create or update workflow`
- `git push` rejected for paths under `.github/workflows/`

**Fix (Josh):** grant the `workflow` scope on the token (classic PAT) or use
the GitHub website / a scoped app to commit the two files from this folder
into `.github/workflows/` on `main`. Then push a tag `v*` (or run
**Release firmware → workflow_dispatch**) to publish binaries.

Until CI is live, build locally:

```bash
pio run -e cheap-black-display
python tools/package_firmware.py   # merged factory image @ 0x0
```

Release assets must be the **merged** bin (bootloader + partitions + app),
not the app-only `firmware.bin`. The app-only image is published alongside
as `…-app.bin` for OTA.
