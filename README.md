# Windows applications on Lingmo

`lingmo-wine` is a native Lingmo SDK window plus an on-demand, unprivileged
Python Wine runner. It uses the distribution Wine packages; it does not install
or download Wine itself, run code as root, or start a permanent service.

Double-click an EXE or MSI to run it directly. `lingmo-wine FILE` and
`lingmo-wine --open FILE` initialize or reuse an environment without opening
the manager or asking the user to select a prefix. MSI files run through
`wine msiexec /i`. A file inside a managed prefix always uses that prefix;
otherwise an existing registration is preferred, then an automatic environment
associated with the canonical file path. Each new external file gets its own
environment. Renaming/moving an external file can create a new environment;
register it explicitly when it must keep a particular environment. If more
than one registration uses the same path, use its launcher entry to choose
unambiguously. On failure the manager opens the log page with the helper error.
Successful launches need no display connection from the manager itself.

Start Windows Applications without a file for advanced environment management.
It supports installation into a selected environment, Wine settings, uninstall,
launcher registration, per-environment stop, and logs. Registered launchers
remain usable from the desktop and Dock.

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

The installed `lingmo-mimeapps.list` makes this the Lingmo desktop default for
EXE/MSI. Existing higher-priority user MIME preferences are preserved; users
with an explicit different default can change it using Open With.

## Status and deployment

This component is maintained in the LingmoOS/lingmo-wine repository.
Publish the `lingmo-wine` binary package to the Lingmo APT archive before using
the Builder package list, or place the locally built deb in
`lingmo-builder/scripts/lingmo-config/common/packages.chroot/` for a local build.
The Builder's amd64 package list and `05-wine-multiarch.chroot` hook are included
with this work. The hook runs only inside the image build, enables i386, then
installs wine32 and 32-bit Mesa/OpenGL/Vulkan libraries. It intentionally fails
if dependency installation fails. The separate simple-cdd pipeline is not changed.

The manager is available through its own desktop file; the package also
provides Lingmo-specific default MIME associations. Its command line also accepts a local path or
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

## Window layout

The window follows the Lingmo application design specification's left/right
material structure: a 224 logical-pixel glass sidebar holds the left-aligned
application title, shared environment selection and page navigation. The right
content and its window-control area are opaque. Install / Run, Registered
applications, Environments and Logs are separate pages; switching pages keeps
input fields, selected environments and running operations alive. Without
compositor glass support only the sidebar falls back to a theme surface.

The headless window test covers light/dark themes, 100%/150% scale, minimum
and expanded sizes, page exclusivity, preserved input, material alpha boundaries,
and clicking the actual Close button. Headless transparent pixels and material
requests do not prove real compositor blur; verify that in a live Lingmo session.

## Application actions and menus

The status bar menu is exported through com.canonical.dbusmenu and registered
as a nonzero single-window ID. The compact titlebar fallback stays visible
until the registrar acknowledges registration. Losing or restarting the
registrar restores it; a failed or timed-out registration never hides it.
Registration replies are guarded by a generation counter. The fallback and
global menu share the same actions as the buttons. Ctrl+O opens the chooser,
Ctrl+L opens logs, and Ctrl+Q uses the same guarded quit action as Close.

Actions are disabled when Wine, an environment, a valid name or an appropriate
file extension is missing, or another task owns the manager. The helper still
validates paths and locks prefixes; UI validation is not a replacement for it.
D-Bus tests exercise successful/rejected registration, owner loss/restart,
disabled/hidden actions and menu-state updates. Real status bar display still
requires a live Lingmo session check.

## Managing installed applications

The application editor now has its own EXE path field, independent of the
installer on Install / Run. Choosing a registered application loads its name,
path and actual environment. Save changes updates that ID, preserving pins.
Adding the same EXE in the same environment again updates the existing entry
instead of making a duplicate. Another environment may register the same EXE
independently. Rename environment changes only its display name.

Remove launcher entry removes only the generated desktop file and registration;
it does not uninstall or delete the EXE or prefix. Undo removal restores the
most recently removed entry in this manager session. Backups also persist in
`lingmo/wine/removed-applications/`; the helper's `restore ID` command can restore
one later if the executable still exists. Use Wine's Uninstall programs to
actually uninstall software. Metadata changes acquire the environment lock
and reject requests while a managed program is running in that environment.

Read environment log loads at most the last 64 KiB asynchronously; switching
environments drops stale results. Inputs and selectors are locked while an
operation runs. Failure automatically opens the log page. The new management
integration test drives the real helper against temporary files to verify
rename/register/edit/remove/undo/create-selection/log-read without Wine.
