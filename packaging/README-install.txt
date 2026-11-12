SOUND CONNECT - WINDOWS X64 (ZIP FALLBACK)

The simplest install is EarbudControls-Setup-<version>.exe from the same release.
This ZIP installs the same signed package with Install.cmd.

1. Extract the entire ZIP to a folder. Do not run setup inside the ZIP.
2. Requirements: Windows 11 build 22621+, x64, Windows Web Experience Pack,
   and Developer Mode enabled in Windows Settings > System > Advanced >
   For developers. Depending on Windows version, Developer Mode may be under
   Privacy & security > For developers. No other developer settings are needed.
3. Run Install.cmd. First installation requests administrator permission ONLY
   to trust the included public certificate. Cancelling leaves the app uninstalled.
4. Pair/connect Sony WF-C710N manually in Windows Bluetooth settings.
5. Open Win + W, choose Add widgets and add WF-C710N. Open Bandwave
   from Start for companion controls. Pairing and widget placement are manual.

If the picker is stale after enabling Developer Mode, close and reopen Win + W.
A Windows sign-out or restart may be needed. Setup does not disable SmartScreen
or change your global PowerShell policy. A self-signed release can produce
Windows download/reputation prompts; check the source and release before trusting.

The Microsoft-signed runtime dependency is included and is installed only when
the required x64 version is absent. No compiler, SDK or source checkout is needed.
The main installer runs as you; only the certificate helper elevates. Do not launch
the entire installer under a different administrator account.

UPDATES: the app checks for updates once a day (it can be turned off in its
settings). Running a newer setup exe or Install.cmd also updates in place.
EQs and settings live in %LOCALAPPDATA%\Earbud Controls and are kept.
The local signing certificate expires 17 October 2027; a replacement certificate
will require a future trust update. No private signing key is included.

UNINSTALL: Windows Settings > Apps > Installed apps > Bandwave > Uninstall.
The setup exe's entry asks whether to remove your EQs and settings; the package
entry from a ZIP install always keeps them in %LOCALAPPDATA%\Earbud Controls.
This removes this app, not Bluetooth pairing, shared runtimes, or earbud settings.
To remove certificate trust too, an administrator can remove ONLY thumbprint
458DC5875423917D74F77A536EBBB42720D1B67A from Local Computer > Trusted People.
Do not remove other certificates or Microsoft framework packages.

WIDGET: Small shows batteries and the sound modes; Medium and Large add your six
main-screen EQs. Bandwave supports Sony WF-C710N only.

Source, issues and releases: https://github.com/KanuTomer/Bandwave
Independent project; not affiliated with Sony or Microsoft. See LICENSE and
THIRD_PARTY_NOTICES.txt; full third-party licenses are in Licenses/.
