# Building, packaging and installing Thruma

All three platforms share one CMake project and one audio engine. JUCE 8.0.15 is downloaded automatically by CMake (FetchContent); to use a local checkout, pass `-DTHRUMA_JUCE_PATH=/path/to/JUCE`.

| Option | Default | Meaning |
|---|---|---|
| `THRUMA_BUILD_PLUGIN` | ON | Build the JUCE plugin. OFF builds only the DSP core + tests (no JUCE needed) |
| `THRUMA_BUILD_TESTS` | ON | Build the DSP test suite and the VST3 host validator |
| `THRUMA_EXTRA_FORMATS` | – | e.g. `"Standalone"` for a standalone app useful for UI work |
| `THRUMA_JUCE_PATH` | – | Local JUCE checkout |

## Windows (10/11, x64)
Requirements: Visual Studio 2022 (Desktop C++), CMake ≥ 3.22, Git. For the installer: [Inno Setup 6](https://jrsoftware.org/isinfo.php).
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
# installer
iscc /DAppVersion=0.1.0 /DSourceDir=%CD%\build\Source\Plugin\Thruma_artefacts\Release\VST3 /DOutputDir=%CD%\dist packaging\windows\Thruma.iss
```
The plugin links the static MSVC runtime, so no redistributable is needed. The CI also produces a **portable zip**: copy `Thruma.vst3` anywhere your DAW scans.

## macOS (11+, universal arm64 + x86_64)
Requirements: Xcode command line tools, CMake ≥ 3.22, Ninja (`brew install cmake ninja`).
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # universal by default
cmake --build build --parallel
ctest --test-dir build --output-on-failure
packaging/macos/build_pkg.sh build/Source/Plugin/Thruma_artefacts/Release/VST3/Thruma.vst3 0.1.0 dist
```
To build a single architecture, pass `-DCMAKE_OSX_ARCHITECTURES=arm64` (or `x86_64`).

### Code signing and notarization
`build_pkg.sh` signs and notarizes when these environment variables are set (the CI reads them from repository secrets of the same names):

| Variable | Purpose |
|---|---|
| `APPLE_CODESIGN_IDENTITY` | `Developer ID Application: …`: signs the plug-in with the hardened runtime |
| `APPLE_INSTALLER_IDENTITY` | `Developer ID Installer: …`: signs the `.pkg` |
| `APPLE_ID`, `APPLE_APP_PASSWORD`, `APPLE_TEAM_ID` | `notarytool` submission + stapling |
| `APPLE_CERTIFICATE_P12`, `APPLE_CERTIFICATE_PASSWORD` | (CI only) base64 `.p12` containing both certificates, imported into a temporary keychain |

Without them the plug-in is ad-hoc signed, which works on the build machine. On other Macs, Gatekeeper blocks unsigned downloads; see *Installing* below.

## Linux (x86-64)
Requirements (Debian/Ubuntu names):
```bash
sudo apt install build-essential cmake ninja-build git libasound2-dev libx11-dev libxrandr-dev \
     libxinerama-dev libxcursor-dev libxext-dev libfreetype6-dev libfontconfig1-dev
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
(cd build && cpack -G DEB)                      # Thruma-x.y.z-Linux-x86_64.deb
```
Fedora: `alsa-lib-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXext-devel freetype-devel fontconfig-devel`. Arch: `alsa-lib libx11 libxrandr libxinerama libxcursor libxext freetype2 fontconfig`.

The plugin has no dependency on JACK, PipeWire or WebKit; it runs in any Linux VST3 host (Reaper, Bitwig, Ardour, Carla, …).

## Installing
| OS | Location | Notes |
|---|---|---|
| Windows | `C:\Program Files\Common Files\VST3\Thruma.vst3` | Run the Setup.exe, or unzip the portable package there. Unsigned installers trigger SmartScreen: *More info → Run anyway* |
| macOS | `/Library/Audio/Plug-Ins/VST3/Thruma.vst3` (or `~/Library/Audio/Plug-Ins/VST3`) | Run the .pkg. For an unsigned build, right-click → Open, or run `xattr -dr com.apple.quarantine Thruma.vst3` after copying it manually |
| Linux | `/usr/lib/vst3` (deb, `--system`) or `~/.vst3` | `sudo apt install ./Thruma-*.deb`, or unpack the .tar.gz and run `./install.sh` |

Then rescan plug-ins in your DAW.

## Continuous integration and releases
`.github/workflows/build.yml` runs on every push and pull request, on Windows, macOS and Linux. Each runner:
1. builds the plugin and runs the DSP test suite;
2. loads the built VST3 with `ThrumaHostValidator` (layouts, sample rates, latency/PDC, state, editor);
3. runs pluginval at strictness level 5;
4. builds the installers (Setup.exe + portable zip / .pkg + zip / .deb + .tar.gz) and uploads them as artifacts.

Pushing a tag `vX.Y.Z` also creates a GitHub Release with all packages attached. Bump the version in the top-level `CMakeLists.txt` first.
