# Simpleminded Sunshine

A fork of [Sunshine](https://github.com/LizardByte/Sunshine) for
[Simpleminded Screens](https://github.com/Studio-Catharsis-Creative-Lab/simpleminded-screens).
It is Sunshine plus a few additions. Name, ports (47984/47989/47990), config file, web UI and pairing
store are unchanged, so stock Moonlight clients work with it exactly as with Sunshine, and existing
pairings carry over.

Licensed GPLv3, like Sunshine.

## What the fork adds

### `GET /clipboard`

The host's clipboard text, for the Simpleminded Screens viewer's "Copy from host"
(`Ctrl+Alt+Shift+C`).

- Served on the paired HTTPS port (47984) only, never on plain HTTP. Sunshine's TLS check rejects
  clients that were not paired by PIN (401) before the request is handled.
- Answers only while a stream is running (`rtsp_stream::session_count() > 0`).
- Reads the clipboard with `wl-paste --no-newline --type text` (Wayland) or
  `xclip -o -selection clipboard` (X11). If Sunshine's service has no `WAYLAND_DISPLAY`, it uses the
  first `wayland-*` socket in `$XDG_RUNTIME_DIR`.
- The reader is killed after 2 s and output is capped at 1 MB, so a stuck clipboard cannot hang
  Sunshine's HTTPS server. Control characters other than tab, newline and carriage return are dropped,
  so the reply is always valid XML.

Replies use the same XML shape as Sunshine's other endpoints:

| status_code | Body / status_message |
|---|---|
| 200 | `<root status_code="200"><text>…</text></root>` |
| 403 | Clipboard is shared only during a stream |
| 404 | The host's clipboard has no text |
| 413 | The host's clipboard is too large (over 1 MB) |
| 500 | Could not read the host's clipboard |
| 501 | Install wl-clipboard on the host / Install xclip on the host |

Any paired client can read the clipboard while any stream is running.

### `<SmsFeatures>` in `serverinfo`

A comma-separated list of what this fork adds (today `clipboard`). The viewer uses it to enable
fork-only features; stock clients ignore unknown elements.

## Where the changes are

| File | Change |
|---|---|
| `src/sms/clipboard.h`, `src/sms/clipboard.cpp` | Reading the clipboard, limits, reply building |
| `src/nvhttp.cpp` | `clipboard` handler, `^/clipboard$` route on the HTTPS server, `SmsFeatures` in `serverinfo` (each tagged with a `Simpleminded Screens` comment) |
| `cmake/compile_definitions/common.cmake` | The two `src/sms` files in `SUNSHINE_TARGET_FILES` |
| `tests/unit/test_sms_clipboard.cpp` | Tests: reply format, status mapping, and the real reader against a fake `wl-paste` (output, empty, over 1 MB, stuck, closes-then-hangs) |
| `packaging/linux/Arch/PKGBUILD` | Package `sunshine-sms`, which provides and conflicts with `sunshine` |
| `SMS.md` | This file |

Run the fork's tests: `build/tests/test_sunshine --gtest_filter='SmsClipboard.*'`.

## Platforms

Linux only for now: `src/sms/clipboard.cpp` uses Linux APIs (`pipe2`, `execvpe`, `close_range`,
which needs glibc 2.34 and kernel 5.9). Windows and macOS builds need a separate implementation first,
and upstream's Windows/macOS CI jobs fail on this branch until then.

## Build and install (Arch, Omarchy)

```bash
sudo pacman -S --needed base-devel boost npm python-jinja appstream-glib wl-clipboard
git clone -b sms https://github.com/Studio-Catharsis-Creative-Lab/simpleminded-sunshine.git
cd simpleminded-sunshine
CLONE_URL=https://github.com/Studio-Catharsis-Creative-Lab/simpleminded-sunshine.git \
COMMIT=$(git rev-parse HEAD) BRANCH=sms BUILD_VERSION=2026.1114.155648 \
cmake -B build-pkg -DSUNSHINE_CONFIGURE_PKGBUILD=ON -DSUNSHINE_CONFIGURE_ONLY=ON
cd build-pkg && makepkg -si --nocheck
systemctl --user restart app-dev.lizardbyte.app.Sunshine
curl -sk http://localhost:47989/serverinfo | grep -o '<SmsFeatures>[^<]*</SmsFeatures>'
```

- `BUILD_VERSION` must be the upstream release tag the branch is on (`git describe --tags`, without the
  `v`). Without it the version is `0.0.0-<sha>`, which Arch rejects.
- `--nocheck` skips the packaged test step, which needs `gcovr`.
- The package builds from the pushed commit, so push `sms` before building.
- It replaces the `sunshine` package; settings and pairings stay. Reinstall `sunshine` to go back.

Ubuntu and other distributions: not packaged yet. Upstream's `scripts/linux_build.sh` should build a
`.deb` from this branch; that is untested.

## Development build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_DOCS=OFF -DBUILD_TESTS=ON \
  -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF
cmake --build build
build/tests/test_sunshine --gtest_brief=1
```

Stop the installed service (`systemctl --user stop app-dev.lizardbyte.app.Sunshine`) before running
`./build/sunshine`; both use the same ports and config.

## Keeping up with upstream

`master` tracks LizardByte/Sunshine unchanged; all fork work is on `sms`. Keep fork code in `src/sms/`
and marked lines, so merges stay easy.

```bash
git remote add upstream https://github.com/LizardByte/Sunshine.git   # once
git fetch upstream
git checkout master && git merge --ff-only upstream/master
git checkout sms && git merge master
```
