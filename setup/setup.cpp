// EarbudControls-Setup.exe: one file that installs, updates and uninstalls Sound Connect.
//
// It carries the signed MSIX (app + widget), the Microsoft Windows App Runtime it depends on, and
// the public certificate the package is signed with, each checked against a SHA-256 fixed at build
// time. Install and update use Windows' own package manager, so an update keeps the same package
// identity and everything the listener saved. Administrator rights are asked for once, only to
// trust the certificate. Setup registers its own "Sound Connect" entry in Installed apps, whose
// uninstall asks before removing the EQs and settings in %LOCALAPPDATA%\Sound Connect.
//
//   EarbudControls-Setup.exe            install, or update in place
//   EarbudControls-Setup.exe /update    the app's updater: only a progress window, unless something fails
//   EarbudControls-Setup.exe /uninstall from the Installed apps entry
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "payload.h" // generated: kVersion, kAppSha256, kRuntimeSha256, kCertSha256, resource ids

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Management::Deployment;

static constexpr wchar_t kTitle[] = L"Sound Connect";
static constexpr wchar_t kPackageName[] = L"Kanu.EarbudControls";
static constexpr wchar_t kPublisher[] = L"CN=EarbudControls.Local";
static constexpr wchar_t kThumbprint[] = L"458DC5875423917D74F77A536EBBB42720D1B67A";
static constexpr wchar_t kRuntimeFamily[] = L"Microsoft.WindowsAppRuntime.2_8wekyb3d8bbwe";
static constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EarbudControls";
static bool quiet = false;

// ---------- Small helpers ----------
static int ask(std::wstring const& heading, std::wstring const& body, TASKDIALOG_COMMON_BUTTON_FLAGS buttons = TDCBF_OK_BUTTON, PCWSTR icon = TD_INFORMATION_ICON) {
    int pressed = 0;
    TaskDialog(nullptr, nullptr, kTitle, heading.c_str(), body.c_str(), buttons, icon, &pressed);
    return pressed;
}
static void fail(std::wstring const& heading, std::wstring const& body) { ask(heading, body, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON); }
// A progress window while Windows installs the package, so an update never looks like the app
// simply vanished. The work runs on its own thread and closes the window when it ends.
struct Progress { std::function<void()> work; std::exception_ptr error; std::atomic<bool> done{false}; };
static HRESULT CALLBACK progress_events(HWND dialog, UINT event, WPARAM, LPARAM, LONG_PTR data) {
    auto& progress = *reinterpret_cast<Progress*>(data);
    if (event == TDN_CREATED) {
        SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        SendMessageW(dialog, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
        std::thread([dialog, &progress] {
            init_apartment();
            try { progress.work(); } catch (...) { progress.error = std::current_exception(); }
            progress.done = true;
            PostMessageW(dialog, TDM_ENABLE_BUTTON, IDCANCEL, TRUE);
            PostMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
        }).detach();
    }
    if (event == TDN_BUTTON_CLICKED) return progress.done ? S_OK : S_FALSE; // stays open until Windows is done
    return S_OK;
}
static void with_progress(std::wstring const& heading, std::wstring const& body, std::function<void()> work) {
    Progress progress; progress.work = std::move(work);
    TASKDIALOGCONFIG config{sizeof(config)};
    config.pszWindowTitle = kTitle;
    config.pszMainInstruction = heading.c_str();
    config.pszContent = body.c_str();
    config.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pfCallback = progress_events;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&progress);
    if (FAILED(TaskDialogIndirect(&config, nullptr, nullptr, nullptr))) progress.work();
    if (progress.error) std::rethrow_exception(progress.error);
}

static fs::path data_folder() {
    PWSTR raw{};
    check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw));
    fs::path path = fs::path(raw) / L"Earbud Controls";
    CoTaskMemFree(raw);
    return path;
}
static std::string resource(int id) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!found) throw std::runtime_error("Setup is missing part of its contents. Download it again.");
    HGLOBAL loaded = LoadResource(nullptr, found);
    return {static_cast<const char*>(LockResource(loaded)), SizeofResource(nullptr, found)};
}
static std::wstring sha256(std::string const& bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    check_hresult(HRESULT_FROM_NT(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)));
    std::array<UCHAR, 32> digest{};
    BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), digest.data(), 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::wstring hex;
    for (auto b : digest) { wchar_t pair[3]; swprintf_s(pair, L"%02X", b); hex += pair; }
    return hex;
}
// Each part is checked before it is written anywhere.
static fs::path unpack(fs::path const& folder, int id, std::wstring const& expected, const wchar_t* name) {
    auto bytes = resource(id);
    if (sha256(bytes) != expected) throw std::runtime_error("Setup's contents don't match what was built. Download it again.");
    auto path = folder / name;
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out.flush()) throw std::runtime_error("Couldn't write setup files to the temporary folder.");
    return path;
}

// ---------- Checks ----------
static DWORD os_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{sizeof(info)};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&info);
    return info.dwBuildNumber;
}
static bool developer_mode() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock", L"AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
}
static std::optional<Windows::ApplicationModel::Package> installed(PackageManager const& manager, std::wstring const& name, std::wstring const& publisher) {
    for (auto const& package : manager.FindPackagesForUser(L"", name, publisher)) return package;
    return std::nullopt;
}
static bool runtime_ready(PackageManager const& manager) {
    for (auto const& package : manager.FindPackagesForUser(L"", kRuntimeFamily)) {
        auto id = package.Id(); auto v = id.Version();
        if (id.Architecture() == Windows::System::ProcessorArchitecture::X64 && (v.Major > 2 || (v.Major == 2 && (v.Minor > 5 || (v.Minor == 5 && v.Build >= 1))))) return true;
    }
    return false;
}
static bool widgets_available(PackageManager const& manager) {
    for (auto const& package : manager.FindPackagesForUser(L"", L"MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy")) { (void)package; return true; }
    return false;
}

// ---------- Certificate trust (the only step that needs administrator rights) ----------
static std::wstring thumbprint(PCCERT_CONTEXT cert) {
    BYTE hash[20]{}; DWORD size = sizeof(hash);
    if (!CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID, hash, &size)) return {};
    std::wstring hex; for (DWORD i = 0; i < size; ++i) { wchar_t pair[3]; swprintf_s(pair, L"%02X", hash[i]); hex += pair; }
    return hex;
}
static bool certificate_trusted() {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, L"TrustedPeople");
    if (!store) return false;
    bool found = false;
    for (PCCERT_CONTEXT cert = nullptr; !found && (cert = CertEnumCertificatesInStore(store, cert)) != nullptr;) found = thumbprint(cert) == kThumbprint;
    CertCloseStore(store, 0);
    return found;
}
static int trust_certificate(fs::path const& file) { // runs elevated, as "/trust <file>"
    std::ifstream in(file, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), {}};
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    if (!cert) return 2;
    const bool expected = thumbprint(cert) == kThumbprint;
    bool added = false;
    if (expected) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPeople");
        if (store) { added = CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_USE_EXISTING, nullptr); CertCloseStore(store, 0); }
    }
    CertFreeCertificateContext(cert);
    return expected && added ? 0 : 3;
}
static bool elevate_trust(fs::path const& cert) {
    wchar_t self[MAX_PATH]{}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring args = L"/trust \"" + cert.wstring() + L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS; info.lpVerb = L"runas"; info.lpFile = self; info.lpParameters = args.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return false; // cancelled at the admin prompt
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(info.hProcess, &code); CloseHandle(info.hProcess);
    return code == 0 && certificate_trusted();
}

// ---------- Installed apps entry ----------
static void set_string(HKEY key, const wchar_t* name, std::wstring const& value) { RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))); }
static void register_entry(fs::path const& setup, uintmax_t bytes) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    set_string(key, L"DisplayName", kTitle);
    set_string(key, L"DisplayVersion", kVersion);
    set_string(key, L"Publisher", L"Sound Connect (independent)");
    set_string(key, L"DisplayIcon", setup.wstring() + L",0");
    set_string(key, L"InstallLocation", setup.parent_path().wstring());
    set_string(key, L"UninstallString", L"\"" + setup.wstring() + L"\" /uninstall");
    set_string(key, L"URLInfoAbout", L"https://github.com/KanuTomer/Sound-Connect");
    DWORD one = 1, size = static_cast<DWORD>(bytes / 1024);
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&size), sizeof(size));
    RegCloseKey(key);
}
static void launch_app(Windows::ApplicationModel::Package const& package) {
    const std::wstring target = L"shell:AppsFolder\\" + std::wstring(package.Id().FamilyName()) + L"!App";
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------- Install or update ----------
static int install() {
    PackageManager manager;
    std::wstring problems;
    if (os_build() < 22621) problems += L"• Windows 11 version 22H2 or newer is needed.\n";
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) problems += L"• This release is for x64 PCs only.\n";
    if (!developer_mode()) problems += L"• Turn on Developer Mode in Settings > System > For developers. Windows only shows widgets from apps outside the Store when it's on.\n";
    if (!widgets_available(manager)) problems += L"• Install or update Windows Web Experience Pack from the Microsoft Store, for Widgets.\n";
    if (!problems.empty()) {
        if (ask(L"Sound Connect can't be installed yet", problems + L"\nOpen the For developers settings now?", TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON) == IDYES)
            ShellExecuteW(nullptr, L"open", L"ms-settings:developers", nullptr, nullptr, SW_SHOWNORMAL);
        return 1;
    }
    auto existing = installed(manager, kPackageName, kPublisher);
    if (existing) {
        auto v = existing->Id().Version();
        const std::wstring current = std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." + std::to_wstring(v.Build);
        if (!quiet && current == kVersion) {
            if (ask(L"Sound Connect " + current + L" is already installed", L"Install it again? Your EQs and settings stay as they are.", TDCBF_YES_BUTTON | TDCBF_NO_BUTTON) != IDYES) return 0;
        }
    }
    const auto temp = fs::temp_directory_path() / (L"EarbudControlsSetup-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(temp);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{temp};
    const auto app = unpack(temp, IDR_APP, kAppSha256, L"EarbudControls.msix");
    const auto cert = unpack(temp, IDR_CERT, kCertSha256, L"EarbudControls.Local.cer");
    if (!certificate_trusted()) {
        if (!quiet && ask(L"Allow Sound Connect' certificate", L"This free release is signed with its own certificate. Windows asks for administrator permission once, only to trust it. Nothing else runs as administrator.", TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON) != IDOK) return 1;
        if (!elevate_trust(cert)) { fail(L"The certificate wasn't trusted", L"Sound Connect was not installed. Run setup again and choose Yes at the administrator prompt."); return 1; }
    }
    auto dependencies = single_threaded_vector<Windows::Foundation::Uri>();
    if (!runtime_ready(manager)) dependencies.Append(Windows::Foundation::Uri(unpack(temp, IDR_RUNTIME, kRuntimeSha256, L"Microsoft.WindowsAppRuntime.2.msix").wstring()));
    // Same identity, newer version: Windows updates in place and keeps app data. The running app
    // and widget provider are closed for the moment it takes.
    DeploymentResult result{nullptr};
    with_progress(existing ? L"Updating Sound Connect" : L"Installing Sound Connect",
                  existing ? L"The app and widget close for a moment and the app reopens when it's done. Your EQs and settings are kept." : L"This takes a few seconds.",
                  [&] { result = manager.AddPackageAsync(Windows::Foundation::Uri(app.wstring()), dependencies, DeploymentOptions::ForceApplicationShutdown).get(); });
    if (!result || FAILED(result.ExtendedErrorCode())) { fail(L"Windows couldn't install Sound Connect", result ? std::wstring(result.ErrorText()) : std::wstring(L"The installation didn't finish.")); return 1; }
    auto now = installed(manager, kPackageName, kPublisher);
    if (!now) { fail(L"Windows couldn't confirm the installation", L"Run setup again. If this keeps happening, restart Windows first."); return 1; }

    // Keep a copy of setup beside the data, for Installed apps to uninstall with.
    wchar_t self[MAX_PATH]{}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto kept = data_folder() / L"Setup" / L"EarbudControls-Setup.exe";
    std::error_code ec;
    fs::create_directories(kept.parent_path(), ec);
    if (fs::path(self) != kept) fs::copy_file(self, kept, fs::copy_options::overwrite_existing, ec);
    register_entry(kept, fs::file_size(app, ec) + fs::file_size(self, ec));

    if (quiet) { launch_app(*now); return 0; }
    if (ask(existing ? L"Sound Connect is up to date" : L"Sound Connect is installed",
            (existing ? std::wstring(L"Version ") + kVersion + L". Your EQs and settings were kept." : std::wstring(L"Add the widget from Win + W > Add widgets > WF-C710N.")) + L"\n\nOpen Sound Connect now?",
            TDCBF_YES_BUTTON | TDCBF_NO_BUTTON) == IDYES) launch_app(*now);
    return 0;
}

// ---------- Uninstall ----------
static int uninstall() {
    BOOL remove_data = FALSE;
    TASKDIALOGCONFIG config{sizeof(config)};
    config.pszWindowTitle = kTitle;
    config.pszMainInstruction = L"Uninstall Sound Connect?";
    config.pszContent = L"The app and its widget are removed from this PC. Bluetooth pairing and the settings stored on your earbuds stay as they are.";
    config.pszVerificationText = L"Also remove my EQs and settings from this PC";
    config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    config.pszMainIcon = TD_WARNING_ICON;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, &remove_data)) || pressed != IDOK) return 0;
    PackageManager manager;
    if (auto package = installed(manager, kPackageName, kPublisher)) {
        auto result = manager.RemovePackageAsync(package->Id().FullName()).get();
        if (FAILED(result.ExtendedErrorCode())) { fail(L"Windows couldn't remove Sound Connect", std::wstring(result.ErrorText())); return 1; }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    // This copy of setup is running from the data folder, so it is deleted once it has exited.
    const auto folder = data_folder();
    const auto target = remove_data ? folder : folder / L"Setup";
    const std::wstring command = L"/c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + target.wstring() + L"\"";
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    ShellExecuteW(nullptr, L"open", (fs::path(system) / L"cmd.exe").c_str(), command.c_str(), nullptr, SW_HIDE);
    ask(L"Sound Connect was removed", remove_data ? L"Your EQs and settings were removed too." : L"Your EQs and settings were kept, in case you install it again.");
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring mode = count > 1 ? args[1] : L"";
    try {
        init_apartment();
        if (mode == L"/trust" && count > 2) return trust_certificate(args[2]);
        if (mode == L"/check" && count > 2) { // read-only: verifies the payload and reports readiness
            std::wofstream out(args[2]);
            bool ok = true;
            for (auto [id, sha, name] : {std::tuple{IDR_APP, kAppSha256, L"app"}, std::tuple{IDR_RUNTIME, kRuntimeSha256, L"runtime"}, std::tuple{IDR_CERT, kCertSha256, L"certificate"}}) {
                const bool match = sha256(resource(id)) == sha; ok = ok && match;
                out << name << L": " << (match ? L"verified" : L"MISMATCH") << L"\n";
            }
            PackageManager manager; SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
            out << L"version: " << kVersion << L"\nwindows build: " << os_build() << L"\nx64: " << (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
                << L"\ndeveloper mode: " << developer_mode() << L"\nwidgets: " << widgets_available(manager) << L"\nruntime installed: " << runtime_ready(manager)
                << L"\ncertificate trusted: " << certificate_trusted() << L"\napp installed: " << installed(manager, kPackageName, kPublisher).has_value() << L"\n";
            return ok ? 0 : 4;
        }
        if (mode == L"/uninstall") return uninstall();
        quiet = mode == L"/update";
        return install();
    } catch (hresult_error const& e) {
        fail(L"Setup stopped", std::wstring(e.message()));
    } catch (std::exception const& e) {
        fail(L"Setup stopped", std::wstring(to_hstring(e.what())));
    }
    return 1;
}
