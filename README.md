# Earbud Controls for Windows

A Windows 11 app and Win + W widget for Sony WF-C710N: batteries, noise cancelling, ambient
sound, your EQ library, Find your EQ and Clear Bass. Independent project; not affiliated with Sony
or Microsoft. Project source is MIT licensed.

## Install

Download **EarbudControls-Setup-0.5.9.exe** from
[Releases](https://github.com/KanuTomer/Sound-Connect/releases) and run it. It installs the app and
the widget together. No compiler, ZIP or source checkout is needed.

Requirements: Windows 11 x64 (build 22621+), Windows Web Experience Pack, and Developer Mode on in
Windows Settings > System > For developers. Windows only shows widgets from apps outside the
Microsoft Store when Developer Mode is on; setup checks this and explains how to turn it on. Other
models and ARM64 are unverified. Pair and connect WF-C710N in Windows Bluetooth settings first.

The free release is signed with a local certificate. The first install asks for administrator
permission once, only to trust that certificate; everything else runs as you. Setup includes the
Microsoft-signed Windows App Runtime 2.5.1 and installs it only if it is missing. It does not
disable SmartScreen, change PowerShell policy, pair devices or change earbud settings. Windows
download and reputation prompts may still appear. The certificate expires 17 October 2027.

Open **Earbud Controls** from Start. Add the widget from Win + W > Add widgets > **WF-C710N**.

- The app: batteries, sound modes, your EQ library with six EQs on the main screen, the EQ editor
  (five bands from 400 Hz to 16 kHz, plus Clear Bass), Find your EQ, and two EQs kept on the
  earbuds' Custom 1 and Custom 2 for your phone or other devices.
- The widget: batteries, the three sound modes and, on Medium and Large, your six main-screen EQs.
- Choosing a Sony preset selects it on the earbuds. Your own EQs are written to the earbuds'
  Manual slot. On the first connection the app imports what is already in Custom 1 and Custom 2.

## Updates and uninstall

The app checks this project's GitHub Releases once a day (Settings > App and widget lets you turn
this off; nothing about you is sent). When a newer version is out, choose **Update** in the title
bar. The update is installed only after its checksum and signature are verified, and your EQs and
settings stay. Running a newer setup yourself does the same.

EQs and settings are kept in `%LOCALAPPDATA%\Earbud Controls`. Uninstall from Settings > Apps >
Installed apps > **Earbud Controls** (the entry setup adds): it asks whether to remove your EQs
and settings too, and keeps them unless you say so. Removing the Windows package entry instead
always keeps them. Neither removes Bluetooth pairing, shared Microsoft runtimes or anything stored
on the earbuds. To remove certificate trust, an administrator can remove only thumbprint
458DC5875423917D74F77A536EBBB42720D1B67A from Local Computer > Trusted People.

A ZIP with Install.cmd remains available as a fallback for each release.

## Build and test

Windows x64, Git, Visual Studio 2022 C++ Build Tools, Windows SDK 10.0.26100 and
CMake 3.31+ are required. In Windows PowerShell from the repository:

```powershell
./scripts/build.ps1 -Check   # owner, widget, app (EarbudControls.UI.exe) and tests
./tests/install_tests.ps1
```

Bootstrap fetches pinned SonyHeadphonesClient revision
`b98309446e2aff7538959eb6518ab694c5264cbe`, applies the WF-C710N EQ-notification
compatibility patch idempotently, downloads pinned Widgets/C++/WinRT packages and
generates build-time projections. Existing incompatible dependencies are left
untouched. CI uses windows-2025 and publishes unsigned debugging builds only.

The production manifest is the authoritative version. Maintainers sign locally;
no private key is uploaded to GitHub or CI. To package an explicitly approved UI:

```powershell
# Build the demo provider and prepare review files without registering an app.
./scripts/register-ui-review.ps1 -PrepareOnly
./scripts/prepare-release.ps1 -UserApprovedUi
```

Use `-UserApprovedUi` only after actual human approval of the current UI. It records
that approval separately from automated native visual checks. The default packaging
gate still requires native review evidence. The development certificate is local
and non-exportable; a new maintainer cannot sign releases with the original key.
The installer pins the original public certificate thumbprint.

For a read-only setup check, run the extracted release helper with `-PackagePath`
and `-CheckOnly`. Existing `-Version` and `-TrustCertificate` options remain available
in the source installer. `scripts/reset-local-install.ps1` is a maintainer-only,
version-specific cleanup tool: it defaults to preview, requires a rollback backup
to execute, and refuses unexpected identities and reparse-point traversal.

## Testing limits and privacy

Noise control, presets, EQ writes and restoration were historically verified on
physical WF-C710N firmware 1.0.5. Passing CI does not establish UI acceptance,
Bluetooth reliability, gaming performance or a fresh-machine installation pass.
This release remains a prerelease until the friend-machine flow is verified.

Runtime state may include the selected Bluetooth address, firmware, cached readings,
preferences and your EQ library. It stays in `%LOCALAPPDATA%\Earbud Controls` and is excluded from
the repository and release. Do not attach raw state or diagnostic logs to public issues.
No telemetry or cloud account is required. The only network request is the optional daily
update check to GitHub Releases.

## Third-party software

See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and the full license copies
included in the release. Microsoft's redistributable packages retain their own
license terms; this project's MIT license does not replace them.
