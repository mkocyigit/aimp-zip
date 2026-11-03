# AIMP Zip Plugin

**English** | [Türkçe](README.tr.md)

A C++ plugin for AIMP 6 (64-bit). It expands `.zip` archives into the playlist and plays the audio files inside them through a `zip://<archive>:<entry>` virtual path. No need to extract the archive first.

## Features

- AIMP itself decides which entries are playable, so every installed decoder is supported (mp3, flac, wav, ogg, opus, m4a, ape, wv, ...).
- Tracker modules (xm/it/mod/s3m/mtm) are played from memory via AIMP's BASS library.
- Both OEM code page (e.g. Turkish CP857) and UTF-8 entry names are displayed correctly.
- Read-only: archives are never modified.

## Build

Requires Visual Studio 2022+ and CMake:

```powershell
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release
```

Output: `build-x64\bin\Release\aimp_zip.dll`

## Install

Close AIMP and copy the DLL to `C:\Program Files\AIMP\Plugins\aimp_zip\aimp_zip.dll` (or run `install_x64.bat` as administrator). Then start AIMP.

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled components.
