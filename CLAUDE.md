# IL2MissionGuard project guidance

IL-2 Mission Guard is a native x64 Windows tray application that saves and creates recoverable snapshots for the IL-2 Great Battles and IL-2 Korea Mission Editors.

## Design constraints

- Keep the production application native Win32/C++ with no managed runtime dependency.
- Use the editors' verified native File > Save command (`WM_COMMAND`, `0x8037`).
- Do not patch, inject into, or synthesize keyboard input for either editor.
- Skip unnamed missions and editors whose main windows are disabled by modal dialogs.
- Wait for all same-basename mission files to stabilize before copying.
- Hash every recovery file with SHA-256 and verify it again before restore.
- Always create a pre-restore safety backup and retain rollback behavior.
- Tests must use temporary files and must never modify installed editors.
- Treat warnings as errors and keep the static MSVC runtime.

## Compatibility

The `Local\IL2MEC.AutoSave.Agent` mutex, `Local\IL2MEC.AutoSave.Stop` event, `.il2mec-autosave.json` metadata format, and `%LOCALAPPDATA%\IL2MEC` storage root are intentional compatibility contracts inherited from IL2MEC. Do not change them without a tested migration plan.

## Build and test

Run `build.ps1`, or build `IL2MissionGuard.sln` in Release/x64 and execute `tests\IL2MissionGuard.Tests\bin\IL2MissionGuard.Tests.exe`.

