# IL2MissionGuard project guidance

IL-2 Mission Guard is a .NET 10 x64 Windows tray application with a WPF UI that saves and creates recoverable snapshots for the IL-2 Great Battles and IL-2 Korea Mission Editors.

## Design constraints

- Keep the production application on .NET 10 with a modern WPF UI and publish it as a self-contained, single-file x64 executable.
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

Run `build.ps1`. It executes the legacy native compatibility tests and the managed regression suite, then publishes the .NET 10 WPF application to `artifacts\release\win-x64`.
