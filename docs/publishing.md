# Publishing

This document describes how `v6emul` is built and published to GitHub. All
automation lives in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml).

## Overview

The [CI workflow](../.github/workflows/ci.yml) is responsible for both continuous
integration and releases. It builds and tests the project on every run, and it
publishes downloadable archives to the GitHub **Releases** page when a version
tag is pushed.

| Purpose | Trigger |
|---------|---------|
| Continuous integration (build + test, upload artifacts) | `workflow_dispatch` (manual) |
| Continuous integration **and** GitHub Release | Push of a tag matching `v*` |

The workflow declares `permissions: contents: write` so the release job can
create GitHub Releases.

## Versioning

The build version is a date stamp derived from the UTC date at workflow run time:

```
<YYYY>.<MM>.<DD>          e.g. 2026.10.05
```

This value is computed once in the `version` job, exported as the
`V6EMUL_VERSION` environment variable, and consumed during CMake configuration.
It names the uploaded artifacts and is embedded in the binary (see
`app/version.h.in` → `v6emul --version`).

> The **release tag** and the **build version** are independent. The tag name
> (for example `v1.0.0`) becomes the release title, while the artifact and binary
> version stays `YYYY.MM.DD`.

When `V6EMUL_VERSION` is not set (a normal local build), CMake falls back to the
last commit date and short hash (`YYYY.MM.DD-<hash>`), then to the project version
(`0.1.0`) if Git metadata is unavailable. See the root `CMakeLists.txt`.

## Workflow Jobs

### 1. `version` — Compute Version

Runs on `ubuntu-latest`. Produces the `version` output used by the other jobs:

```bash
VERSION="$(date -u +'%Y.%m.%d')"
```

### 2. `build-test` — Build and Test

Depends on `version`. Runs a fail-fast-disabled matrix across three platforms:

| `os` | `platform` |
|------|------------|
| `windows-latest` | `windows-x86_64` |
| `ubuntu-latest` | `linux-x86_64` |
| `macos-latest` | `macos-x86_64` |

Each matrix entry:

1. **Configure** — `cmake --preset ci`
2. **Build** — `cmake --build --preset ci --config Release`
3. **Test** — `ctest --test-dir build/ci --build-config Release --output-on-failure --timeout 120`
4. **Upload binary** — publishes an artifact named
   `v6emul-<version>-<platform>` containing the emulator executable:

   | Platform | Path |
   |----------|------|
   | Windows | `build/ci/app/Release/v6emul.exe` |
   | Linux/macOS | `build/ci/app/v6emul` |

Artifacts are uploaded with `if-no-files-found: error`, so a missing binary
fails the job rather than producing an empty archive.

### 3. `release` — Publish Release

Depends on `version` and `build-test`. Runs only when the ref is a version tag:

```yaml
if: startsWith(github.ref, 'refs/tags/v')
```

Steps:

1. **Download all artifacts** into `artifacts/`.
2. **Package artifacts** into `dist/`:
   - Windows → `<name>.zip` (`zip -r`)
   - Linux/macOS → `<name>.tar.gz` (`tar -czf`), with the executable bit set on
     `v6emul` first
3. **Create GitHub Release** via `softprops/action-gh-release@v2`:
   - `tag_name` = `${{ github.ref_name }}`
   - `name` = `${{ github.ref_name }}`
   - `generate_release_notes: true`
   - attaches everything in `dist/*`

## Cutting a Release

Releases are tag-driven. From a clean checkout of the branch you want to ship:

```bash
# 1. Make sure the changelog/state you want to release is committed and pushed.
git status
git push origin main

# 2. Create and push a version tag. The 'v' prefix is required.
git tag v1.0.0
git push origin v1.0.0
```

Pushing the `v1.0.0` tag triggers the full pipeline: all platform builds run,
tests must pass, and if every platform succeeds the release job publishes a
GitHub Release with the platform archives attached.

The corresponding releases are listed at
<https://github.com/parallelno/v6emul/releases>.

## Manual (CI-only) Run

Use **Actions → CI → Run workflow** (the `workflow_dispatch` trigger) to build
and test without publishing. This runs `version` and `build-test` only — the
`release` job is skipped because no version tag is present. The per-platform
binaries are available as downloadable workflow artifacts.

## Release Artifacts

A successful tagged run produces archives of this form:

| File | Platform |
|------|----------|
| `v6emul-<YYYY.MM.DD>-windows-x86_64.zip` | Windows x86_64 |
| `v6emul-<YYYY.MM.DD>-linux-x86_64.tar.gz` | Linux x86_64 |
| `v6emul-<YYYY.MM.DD>-macos-x86_64.tar.gz` | macOS x86_64 |

Each archive contains the emulator executable (`v6emul.exe` or `v6emul`). Users
extract the archive and run the binary directly, or add the extracted directory
to their `PATH`.

## Reproducing the CI Build Locally

The `ci` preset mirrors the workflow build exactly:

```bash
cmake --preset ci
cmake --build --preset ci --config Release
ctest --test-dir build/ci --build-config Release --output-on-failure --timeout 120
```

To embed the same version stamp the workflow uses, set the environment variable
before configuring (PowerShell):

```powershell
$env:V6EMUL_VERSION = (Get-Date -AsUTC -Format 'yyyy.MM.dd')
cmake --preset ci
cmake --build --preset ci --config Release
```

See [Building](building.md) for the full build reference.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Release job skipped | Ref is not a `v*` tag | Push a tag such as `v1.0.0` |
| `Upload binary` step fails | Binary not found at the expected path | Verify the `ci` preset's output layout in [Building](building.md#output) |
| Tests fail on one platform | Platform-specific behavior | The matrix is `fail-fast: false`; inspect that platform's log before re-tagging |
| Release has no files | Artifact names/paths changed | Keep artifact `path` entries in sync with `build/ci/app/...` |
| Attempting to re-push a tag | Tag already exists | Delete the remote tag (`git push origin :v1.0.0`) and recreate it, or bump to a new version |
