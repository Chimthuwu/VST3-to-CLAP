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
- [x] v1.0.0 release with Linux + Windows assets (hand-packed from `bin/`)
- [x] Fix the MSVC build (`NOMINMAX`); all three CI platforms green
- [x] v1.0.1 cut from the green commit, CI publishes all three platforms

## Next

- [ ] **BUG (found 2026-10-24, Windows):** CI-built wrapper (57ea38f) converts fine
      (`vst3-to-clap.exe` on real VST3s) but `clap-test-host.exe` fails on every
      plug-in tried (7 JUCE-based VST3s) with "Could not create component"
      (`src/plugin.cpp:251`, `factory.createInstance` returns null). Investigate
      (COM/OLE init? module load path? needs a non-JUCE control such as an MDA build)
- [ ] Idiot-proofing review of `tools/vst3-to-clap-ui.html` (auto-detect OS/arch,
      one-click install, where-to-put-the-.clap guide, macOS re-sign helper)
- [ ] AAX feasibility write-up (SDK is closed/NDA — confirm)

- [ ] Test the Windows and macOS builds in a real host. Both now compile in CI
      (MSVC and Apple Clang), but the README's "never run in anger" caveat
      still stands — nothing has loaded them in Bitwig or REAPER yet
- [ ] Confirm the macOS `.clap` is genuinely universal (`lipo -info`) and decide
      whether to ship it ad-hoc signed or properly notarised

## Later

- [ ] CLAP extensions left unimplemented: `audio-ports-config`, `note-name`,
      `preset-load`, `remote-controls`
- [ ] Expose VST3 program lists as something better than a program-change param
- [ ] Topics/description on the repo once the shape of it settles

## Open questions

- Keep `bin/` out of git permanently, or commit a Linux binary for convenience?
  Currently out, on the grounds that 4.5 MB of binaries in history is hard to
  undo and Releases already solve it.
- Version is `1.0.1` in `CMakeLists.txt` despite macOS/Windows being untested
  in a host. Worth a `0.x` line instead? v1.0.0 was the initial drop (Linux +
  MinGW Windows); v1.0.1 is the first release built entirely by CI.
