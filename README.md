# Windows applications on Lingmo

`lingmo-wine` is a native Lingmo SDK window plus an on-demand, unprivileged
Python Wine runner. It uses the distribution Wine packages; it does not install
or download Wine itself, run code as root, or start a permanent service.

Open an EXE/MSI via the file manager's **Open With → Windows Applications**, or
start the manager from the launcher. Select **Create environment**, then
**Browse… → Install / Run**. Use a separate 64-bit prefix per application,
or share an environment for programs that need the same registry. Once installed,
select its executable in the prefix's `drive_c` and **Add to launcher**; the
resulting entry appears in the launcher and can be pinned to Dock. `Wine
settings` opens winecfg, `Uninstall programs` opens Wine's uninstaller, and
`Stop environment` kills only that prefix's wineserver. Logs are in
`~/.local/share/lingmo/wine/logs`. The manager remains open during a running
program, so the operation can report its exit status. Launcher entries use a
helper process without showing the manager window.

A selected Windows installer may install application files under the prefix;
look for its EXE in `environments/<id>/prefix/drive_c/` afterward. The helper
passes arguments as an array, without a shell. `.exe` executes with `wine`,
`.msi` uses `wine msiexec /i`. Prefixes initialize with `wineboot --init`,
are bound to one ID and architecture (`win64`), and are locked against a
second launch until `wineserver -w` completes. No automatic Winetricks/DXVK
or executable download is performed. Wine compatibility depends on the app.

## Build

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

Debian packaging builds `lingmo-wine` for amd64. `wine32:i386` is recommended
for 32-bit Windows binaries; the Builder Live image enables i386 multiarch and
installs it. A 64-bit-only host can run supported 64-bit executables, but
32-bit support should be verified against the chosen Wine package version.
Xwayland is needed for Wine's Windows to show inside the Wayland session.

Installing this package adds file associations but does not force a default
handler for EXE/MSI. A file with another handler can still use Open With.

## Status and deployment

This component is maintained in the LingmoOS/lingmo-wine repository.
Publish the `lingmo-wine` binary package to the Lingmo APT archive before using
the Builder package list, or place the locally built deb in
`lingmo-builder/scripts/lingmo-config/common/packages.chroot/` for a local build.
The Builder's amd64 package list and `05-wine-multiarch.chroot` hook are included
with this work. The hook runs only inside the image build, enables i386, then
installs wine32 and 32-bit Mesa/OpenGL/Vulkan libraries. It intentionally fails
if dependency installation fails. The separate simple-cdd pipeline is not changed.

The manager is available through its own desktop file; no system-wide default
MIME handler is overwritten. Its command line also accepts a local path or
`--open FILE`, and `--launch ID` starts a registered application without an
extra manager window. AppRuntime launches these desktop entries normally;
Windows-window matching and per-program Dock icons depend on Wine's window
classes and still need desktop validation. Automatic shortcut discovery and
EXE icon extraction are not implemented.

Prefixes separate settings and installed files; they are **not security sandboxes**.
They do not restrict access to the user's host files. No VM/guest isolation is
claimed. Environment deletion and host package management are deliberately not
part of the GUI. Wine may show its own prompts for optional components.

Validation performed: native SDK build, Debian binary packaging, desktop-file
validation, twelve mocked Wine runner tests, translation validation, headless
UI captures, and mocked Builder tests on amd64/arm64/i386. No real Wine runtime
or Windows executable was installed or run on the host. Actual EXE/MSI,
32-bit/64-bit behavior, graphics acceleration and live ISO installation remain
acceptance tests for a Wine-enabled system.
