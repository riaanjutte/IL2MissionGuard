# Contributing

Contributions are welcome through GitHub issues and pull requests.

## Development requirements

- Windows 10 or newer
- Visual Studio 2022 with the x64 C++ toolchain
- Windows SDK

Run `build.ps1` before submitting a pull request. The build treats warnings as errors and runs the native regression suite.

## Safety expectations

- Do not replace the editor's Save behavior with simulated input or code injection.
- Keep Save and editor-close operations bounded by timeouts.
- Preserve snapshot hash validation, pre-restore safety backups and rollback behavior.
- Never trust paths or filenames read from recovery metadata without containment checks.
- Preserve the documented legacy compatibility contracts unless the change includes a tested migration path.
- Tests must operate on temporary files and must not modify installed Mission Editors.

Please describe user-visible changes and add regression coverage for changes to snapshot creation, integrity validation, retention or restoration.

