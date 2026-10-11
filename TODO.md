# TODO — vst3-to-clap

A CLAP plug-in that hosts VST3 plug-ins, plus a converter that stamps out a
dedicated `.clap` for any `.vst3`. C++17 / CMake, MIT. Repo:
`Chimthuwu/vst3-to-clap` (public).

## Status

Published 2026-10-12 from `vst3-to-clap.zip`. Source, docs and CI are in the
repo; prebuilt binaries are gitignored and ship as Release assets instead.

## Now

- [x] Extract archive, flatten to repo root
- [x] Gitignore `bin/`; binaries go to Releases
- [x] CI: tag-triggered release job, human-readable asset names
- [x] Reference the two `docs/*.png` screenshots from the README
- [x] `git init`, public repo, push
- [x] v1.0.0 release with Linux + Windows assets

## Next

- [ ] Watch the first CI run on all three runners — only Linux has ever been
      built locally; MSVC and Apple Clang are unverified in practice
- [ ] Once CI produces a macOS universal build, add it to the v1.0.0 release
      (currently absent — `bin/` only had Linux and Windows)
- [ ] Test the Windows binary in a real host. It was cross-built with MinGW and
      the README flags the Windows/macOS paths as never run in anger

## Later

- [ ] CLAP extensions left unimplemented: `audio-ports-config`, `note-name`,
      `preset-load`, `remote-controls`
- [ ] Expose VST3 program lists as something better than a program-change param
- [ ] Topics/description on the repo once the shape of it settles

## Open questions

- Keep `bin/` out of git permanently, or commit a Linux binary for convenience?
  Currently out, on the grounds that 4.5 MB of binaries in history is hard to
  undo and Releases already solve it.
- Version is `1.0.0` in `CMakeLists.txt` despite macOS/Windows being untested.
  Worth a `0.x` line instead? The tag went out as v1.0.0 to match the source.
