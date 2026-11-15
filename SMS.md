# Simpleminded Sunshine

A fork of [Sunshine](https://github.com/LizardByte/Sunshine) for
[Simpleminded Screens](https://github.com/Studio-Catharsis-Creative-Lab/simpleminded-screens).
It is Sunshine plus a few additions; stock Moonlight clients work with it as with Sunshine.

## Additions

- `GET /clipboard` on the paired HTTPS port (47984): the host's clipboard text, only while a stream
  is running. Linux needs `wl-clipboard` (Wayland) or `xclip` (X11).
- `serverinfo` lists them in `<SmsFeatures>`, comma-separated.

All fork code lives in `src/sms/`, plus a few marked lines in `src/nvhttp.cpp`.

## Build and install (Arch, Omarchy)

```bash
cmake -B build-pkg -DSUNSHINE_CONFIGURE_PKGBUILD=ON -DSUNSHINE_CONFIGURE_ONLY=ON
cd build-pkg && makepkg -si
```

This replaces the `sunshine` package; settings and pairings stay. Reinstall `sunshine` to go back.

## Upstream

`master` tracks LizardByte/Sunshine; our work is on `sms`.
`git fetch upstream && git checkout master && git merge --ff-only upstream/master && git checkout sms && git merge master`
