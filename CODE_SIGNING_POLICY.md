# Code signing policy

Free code signing is provided by [SignPath.io](https://signpath.io/), with a certificate issued by the [SignPath Foundation](https://signpath.org/).

The SignPath Foundation application is currently pending. Releases created before approval are unsigned. After approval, only artifacts produced and signed through the controlled process below will be described as signed releases.

## Scope

Only `IL2MissionGuard.exe` built from this repository is eligible for the project's SignPath signing configuration. Third-party or locally supplied binaries must not be submitted under this project.

## Team roles

- Committer and reviewer: [@riaanjutte](https://github.com/riaanjutte)
- Signing-request approver: [@riaanjutte](https://github.com/riaanjutte)

Changes from contributors who do not have commit access require review before merging. A signing request also requires explicit approval in SignPath before the signed artifact is released.

## Build and release controls

1. Release source is identified by a version tag in the public GitHub repository.
2. A GitHub-hosted Windows runner checks out that exact revision.
3. `build.ps1` builds the native tray host and .NET 10 WPF interface with warnings treated as errors, then runs the native and managed regression suites.
4. GitHub Actions stores the unsigned executable as a workflow artifact before submitting its immutable artifact ID to SignPath.
5. SignPath verifies the GitHub build origin and applies an Authenticode signature only after manual approval under the release signing policy.
6. The signed executable is returned to GitHub Actions, verified, and published without further modification.

The private signing key is generated and held by SignPath in a hardware security module. Project maintainers do not receive or store the key.

## Privacy

Mission Guard's network behavior and local-data handling are documented in the [privacy policy](PRIVACY.md).

## Verification

After signed releases begin, users can inspect a downloaded executable in PowerShell:

```powershell
Get-AuthenticodeSignature .\IL2MissionGuard.exe | Format-List Status, StatusMessage, SignerCertificate, TimeStamperCertificate
```

A valid release must report `Status: Valid` and a SignPath Foundation signer. The SHA-256 digest shown by GitHub must also match the downloaded asset.
