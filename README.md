# Improved Wheel Menu (Oblivion Remastered)

An OBSE64 plugin that makes Oblivion Remastered's own quick-key radial a full wheel menu: eight slots that each hold
several items (LT / RT choose, RB or A use), a Magic wheel of its own for spells, and an ammo wheel for arrows while a
bow is held. The game's radial, prompts and quick keys are used throughout. The package README is
[`dist/README.txt`](dist/README.txt); the history is [`CHANGELOG.md`](CHANGELOG.md).

## Build

Requires [XMake](https://xmake.io) 3.0+ and a C++23 compiler (MSVC or clang-cl).

```bat
git clone --recurse-submodules https://github.com/ApocryphaRealm/ImprovedWheelMenuOR
cd ImprovedWheelMenuOR
xmake build
```

The DLL is written to `build/windows/x64/releasedbg/ImprovedWheelMenu.dll`; the shipped files are under `dist/`.

## Licence

GPL-3.0-or-later ([`dist/LICENSE`](dist/LICENSE), [`dist/NOTICE.md`](dist/NOTICE.md)). Built on
[CommonLibOB64](https://github.com/libxse/commonlibob64) and its plugin template (GPL-3.0, with the modding exception in
`EXCEPTIONS`); other linked components are listed in [`dist/THIRD_PARTY_NOTICES.md`](dist/THIRD_PARTY_NOTICES.md).
