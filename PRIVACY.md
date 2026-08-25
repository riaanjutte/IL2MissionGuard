# Privacy policy

IL-2 Mission Guard does not collect analytics, advertising identifiers, mission contents, filenames, recovery-point contents, or personal information for its maintainers.

Mission files, settings, recovery points, integrity hashes, and diagnostic logs remain on the user's computer. They are not uploaded by Mission Guard.

Mission Guard makes HTTPS requests to GitHub's public Releases API after startup and when the user manually checks for updates. These requests disclose ordinary network metadata to GitHub, including the user's IP address and the Mission Guard version in the HTTP user-agent string. If the user approves an update, Mission Guard downloads the selected release executable from GitHub and verifies its published SHA-256 digest before installation. GitHub processes these requests under the [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement).

Mission Guard does not communicate with SignPath during normal use. SignPath is used only in the maintainers' release pipeline to sign published executables.

Users may inspect or delete Mission Guard's local data under `%LOCALAPPDATA%\IL2MissionGuard`. Deleting the `Autosave` subdirectory permanently removes its recovery points.

Questions about this policy may be opened as a GitHub issue. Security-sensitive questions should use GitHub's private vulnerability-reporting feature.
