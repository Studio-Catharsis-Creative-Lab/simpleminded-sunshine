# Simpleminded Sunshine

A fork of [Sunshine](https://github.com/LizardByte/Sunshine) for
[Simpleminded Screens](https://github.com/Studio-Catharsis-Creative-Lab/simpleminded-screens).
It is Sunshine plus a few additions; stock Moonlight clients work with it as with Sunshine.

## Additions

- `GET /clipboard` on the paired HTTPS port (47984): the host's clipboard text, only while a stream
  is running. Linux needs `wl-clipboard` (Wayland) or `xclip` (X11).
- `serverinfo` lists them in `<SmsFeatures>`, comma-separated.
- The fork builds on Linux only for now: `src/sms/clipboard.cpp` uses Linux APIs (`pipe2`, `execvpe`,
  `close_range`). Windows and macOS builds need a separate implementation first.

All fork code lives in `src/sms/`, plus a few marked lines in `src/nvhttp.cpp`.

## Build and install (Arch, Omarchy)

```bash
sudo pacman -S --needed boost npm python-jinja appstream-glib
CLONE_URL=https://github.com/Studio-Catharsis-Creative-Lab/simpleminded-sunshine.git \
COMMIT=$(git rev-parse HEAD) BRANCH=sms BUILD_VERSION=2026.1114.155648 \
cmake -B build-pkg -DSUNSHINE_CONFIGURE_PKGBUILD=ON -DSUNSHINE_CONFIGURE_ONLY=ON
cd build-pkg && makepkg -si --nocheck
```

`BUILD_VERSION` must be the upstream release tag the branch is on (Arch rejects the default `0.0.0-<sha>`).
`--nocheck` skips the test step, which needs `gcovr`. The package builds from the pushed commit, so push first.

This replaces the `sunshine` package; settings and pairings stay. Reinstall `sunshine` to go back.

## Upstream

`master` tracks LizardByte/Sunshine; our work is on `sms`.
`git fetch upstream && git checkout master && git merge --ff-only upstream/master && git checkout sms && git merge master`
