# IL-2 Mission Guard

IL-2 Mission Guard is a lightweight native Windows tray application that protects work in the IL-2 Great Battles and IL-2 Korea Mission Editors.

It asks the editor to run its normal **File > Save** command at a configurable interval, waits for the complete mission family to finish writing, and creates timestamped recovery points that can be restored from the tray menu.

The application was originally developed as the autosave component of IL2MEC. It is now maintained as an independent open-source project. Compatibility identifiers and storage locations are intentionally retained so existing IL2MEC settings and recovery points continue to work.

## Features

- Supports `STEditor.exe` from IL-2 Great Battles and `IL2Editor.exe` from IL-2 Korea.
- Uses the editors' native Save command instead of simulated keyboard input or code injection.
- Saves every same-basename mission companion, including `.Mission`, `.msnbin`, localization, list and metadata files.
- Waits for mission-family writes to stabilize before creating a recovery point.
- Keeps a configurable number of timestamped snapshots per mission.
- Stores a SHA-256 checksum and length for every saved file.
- Shows damaged recovery points as disabled instead of attempting an unsafe restore.
- Creates a separate safety backup before replacing current mission files.
- Closes and reopens the matching editor when restoring a mission.
- Imports legacy recovery points without deleting the old copies.
- Provides a live status window showing detected editors, open missions, the next and most recent recovery points, stored-point count and the last failure.
- Uses a colour-coded tray badge: grey while idle or disabled, yellow while waiting, green once protected and red after a failed recovery attempt.
- Reports the result of every manual recovery-point request instead of silently skipping it.
- Allows routine success notifications to be muted while always showing failures.
- Supports System, Dark, and Light appearances for its status window, settings, controls, title bars, and tray menus.
- Checks the official GitHub Releases feed for updates without blocking the tray application.
- Downloads only the expected release executable and verifies GitHub's published SHA-256 digest before installation.
- Uses a native Win32 tray process with no .NET or WinForms dependency.

The native process typically uses about **10 MB of working memory** and **1–2 MB of private memory** while idle.

## Using Mission Guard

Run `IL2MissionGuard.exe`. Its shield icon appears in the Windows notification area.

Mission Guard defaults to:

- autosave enabled for both editors;
- saving every five minutes;
- keeping ten recovery points per mission; and
- showing a notification after each successful recovery point (failures are always shown).

A new or `<empty>` mission needs to be saved manually once so it has a filename. Mission Guard deliberately skips unnamed missions to avoid opening an unattended Save As dialog.

Settings provides independent toggle switches for Great Battles and Korea autosave. Turning both editor switches off disables autosave completely. The successful-recovery notification has its own independent toggle; failure notifications are always shown.

Right-click the tray icon to:

- open the Mission Guard status window;
- create a recovery point immediately;
- restore a previous recovery point;
- open **Settings...** to configure every autosave option;
- open the recovery-point folder;
- open the diagnostic log;
- check for updates or install an available update; or
- exit Mission Guard.

Double-click the tray icon to open the live status window. You can also use `IL2MissionGuard.exe --status`, or open Settings directly with `IL2MissionGuard.exe --settings`. If Mission Guard is already running, either command opens the requested window in the existing tray process.

## Configuration

For compatibility with existing IL2MEC installations, settings are read from:

```text
%LOCALAPPDATA%\IL2MEC\IL2MEC.ini
```

The relevant section is:

```ini
[AutoSave]
Enabled=true
GreatBattles=true
Korea=true
IntervalMinutes=5
HistoricSnapshots=10
TrayNotifications=true
Theme=System
```

`IntervalMinutes` accepts 1–60, `HistoricSnapshots` accepts 1–100, and `Theme` accepts `System`, `Dark`, or `Light`. The System setting follows the Windows app theme. IL2MEC can continue to manage these settings and install the compatibility build automatically.

For isolated testing, set `IL2MISSIONGUARD_SETTINGS_FILE` to an alternative INI path. The legacy `IL2MEC_SETTINGS_FILE` override is also accepted.

## Updates

Mission Guard checks the repository's latest stable GitHub release after startup. The check runs in the background. When a newer version is available, a tray notification and an **Install update** menu item appear. You can also choose **Check for updates...** at any time.

`IL2MissionGuard.exe --check-updates` opens the same manual update check in a running tray process.

Installation always requires confirmation. Mission Guard downloads the exact `IL2MissionGuard.exe` release asset over HTTPS, verifies it against the SHA-256 digest published by GitHub, replaces the running executable after it exits, and restarts it. Update failures leave the existing executable intact and are written to the diagnostic log.

## Storage

Recovery points and diagnostics remain in the established locations:

```text
%LOCALAPPDATA%\IL2MEC\Autosave
%LOCALAPPDATA%\IL2MEC\autosave.log
```

Older `%TEMP%\STEditor\Autosave` content is imported non-destructively. Restore safety copies are kept beneath `Autosave\RecoveryBeforeRestore`.

## Build

Requirements:

- Windows 10 or newer;
- Visual Studio 2022 Build Tools with the Desktop development with C++ workload; and
- the Windows 10 or 11 SDK.

Build and test from PowerShell:

```powershell
.\build.ps1
```

Or build the projects directly:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" .\IL2MissionGuard.sln /p:Configuration=Release /p:Platform=x64
& .\tests\IL2MissionGuard.Tests\bin\IL2MissionGuard.Tests.exe
```

Warnings are treated as errors. Production builds use C++20, Unicode, `/utf-8`, the static MSVC runtime and only Windows system libraries.

## Safety model

- Mission Guard does not patch or inject code into either Mission Editor.
- The Save message has a bounded timeout and is skipped while a modal dialog disables the editor window.
- Recovery metadata is validated before use, and stored files are rehashed immediately before restore.
- A restore never begins until the matching editor has closed.
- Current files are backed up before replacement, and a failed replacement attempts an automatic rollback.
- Snapshot paths and filenames are constrained to their expected directories to prevent metadata path traversal.
- Update metadata and downloads are accepted only from the project's expected GitHub release URLs, and the downloaded executable must pass SHA-256 verification before it can run.

## Compatibility contracts

The following legacy names are deliberate and should not be changed without a migration plan:

- mutex: `Local\IL2MEC.AutoSave.Agent`
- stop event: `Local\IL2MEC.AutoSave.Stop`
- metadata suffix: `.il2mec-autosave.json`
- settings and recovery root: `%LOCALAPPDATA%\IL2MEC`

They allow the standalone project and IL2MEC-distributed compatibility build to share one safe recovery history and prevent duplicate agents.

## License

IL-2 Mission Guard is available under the [MIT License](LICENSE).

IL-2 Sturmovik and related names are trademarks of their respective owners. This project is an independent community tool and is not affiliated with or endorsed by 1C Game Studios.
