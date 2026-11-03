#pragma once
// Update checks against the project's GitHub Releases, at most once a day and only when the
// listener has left "Check for updates" on. Nothing about the user or the PC is sent. A newer
// setup is run only after its SHA-256 matches the published .sha256, its Authenticode signature is
// intact, and it is signed by the same certificate as this app.
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include "../src/library.hpp"

namespace earbud::updates {
inline constexpr wchar_t kLatest[] = L"https://api.github.com/repos/KanuTomer/Sound-Connect/releases/latest";
inline constexpr wchar_t kThumbprint[] = L"458DC5875423917D74F77A536EBBB42720D1B67A";

// The app's own preferences share this file: update checks and closing to the tray.
struct Settings { bool check = true; int64_t last_check = 0; bool tray = true; };
inline std::filesystem::path settings_path() { return data_folder() / L"app-settings.json"; }
inline Settings load_settings() {
    Settings s;
    json::JsonObject root{nullptr};
    if (json::JsonObject::TryParse(winrt::hstring(read_text(settings_path(), 65536)), root)) {
        s.check = root.GetNamedBoolean(L"check_updates", true);
        s.last_check = static_cast<int64_t>(root.GetNamedNumber(L"last_update_check", 0));
        s.tray = root.GetNamedBoolean(L"close_to_tray", true);
    }
    return s;
}
inline void save_settings(Settings const& s) {
    json::JsonObject root;
    root.SetNamedValue(L"check_updates", json::JsonValue::CreateBooleanValue(s.check));
    root.SetNamedValue(L"last_update_check", json::JsonValue::CreateNumberValue(static_cast<double>(s.last_check)));
    root.SetNamedValue(L"close_to_tray", json::JsonValue::CreateBooleanValue(s.tray));
    write_atomic(settings_path(), root.Stringify());
}
inline int64_t now_seconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// "0.5.1" or "v0.5.1" -> {0,5,1}; anything else is rejected.
inline std::optional<std::array<int, 3>> parse_version(std::wstring text) {
    if (!text.empty() && (text[0] == L'v' || text[0] == L'V')) text.erase(0, 1);
    std::array<int, 3> v{}; size_t at = 0;
    for (int part = 0; part < 3; ++part) {
        size_t used = 0;
        if (at >= text.size() || !iswdigit(text[at])) return std::nullopt;
        try { v[static_cast<size_t>(part)] = std::stoi(text.substr(at), &used); } catch (...) { return std::nullopt; }
        at += used;
        if (part < 2) { if (at >= text.size() || text[at] != L'.') return std::nullopt; ++at; }
    }
    return at == text.size() ? std::optional{v} : std::nullopt;
}

struct Release { std::wstring version, setup_url, sha_url; };

// The newest release, if it is newer than `current` and carries a setup and its checksum.
inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> latest_json() {
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"EarbudControls");
    client.DefaultRequestHeaders().Accept().ParseAdd(L"application/vnd.github+json");
    co_return co_await client.GetStringAsync(winrt::Windows::Foundation::Uri(kLatest));
}
inline std::optional<Release> newer_release(winrt::hstring const& body, std::array<int, 3> current) {
    json::JsonObject root{nullptr};
    if (!json::JsonObject::TryParse(body, root)) return std::nullopt;
    auto version = parse_version(std::wstring(root.GetNamedString(L"tag_name", L"")));
    if (!version || *version <= current || root.GetNamedBoolean(L"draft", false)) return std::nullopt;
    Release release;
    release.version = std::to_wstring((*version)[0]) + L"." + std::to_wstring((*version)[1]) + L"." + std::to_wstring((*version)[2]);
    const std::wstring setup = L"EarbudControls-Setup-" + release.version + L".exe";
    auto assets = root.GetNamedArray(L"assets", json::JsonArray());
    for (uint32_t i = 0; i < assets.Size(); ++i) {
        auto asset = assets.GetObjectAt(i);
        const std::wstring name{asset.GetNamedString(L"name", L"")}, url{asset.GetNamedString(L"browser_download_url", L"")};
        if (!url.starts_with(L"https://github.com/KanuTomer/Sound-Connect/releases/download/")) continue;
        if (name == setup) release.setup_url = url;
        if (name == setup + L".sha256") release.sha_url = url;
    }
    if (release.setup_url.empty() || release.sha_url.empty()) return std::nullopt;
    return release;
}

inline std::wstring file_sha256(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), {}};
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::array<UCHAR, 32> digest{};
    BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest.data(), 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::wstring hex; for (auto b : digest) { wchar_t pair[3]; swprintf_s(pair, L"%02X", b); hex += pair; }
    return hex;
}
// The signature must be intact (a tampered file fails with a bad digest), and its signer must be
// this project's certificate. A self-signed root is expected, so an untrusted-root result is fine.
inline bool signed_by_us(std::filesystem::path const& file) {
    WINTRUST_FILE_INFO info{sizeof(info)}; info.pcwszFilePath = file.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_FILE; data.pFile = &info; data.dwStateAction = WTD_STATEACTION_VERIFY;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (status != ERROR_SUCCESS && status != CERT_E_UNTRUSTEDROOT && status != CERT_E_CHAINING) return false;
    HCERTSTORE store{}; HCRYPTMSG message{}; DWORD encoding{}, content{}, format{};
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format, &store, &message, nullptr)) return false;
    bool match = false;
    DWORD size = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size)) {
        std::string buffer(size, '\0');
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size)) {
            auto signer = reinterpret_cast<PCMSG_SIGNER_INFO>(buffer.data());
            CERT_INFO wanted{}; wanted.Issuer = signer->Issuer; wanted.SerialNumber = signer->SerialNumber;
            if (auto cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &wanted, nullptr)) {
                BYTE hash[20]{}; DWORD length = sizeof(hash);
                if (CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID, hash, &length)) {
                    std::wstring hex; for (DWORD i = 0; i < length; ++i) { wchar_t pair[3]; swprintf_s(pair, L"%02X", hash[i]); hex += pair; }
                    match = hex == kThumbprint;
                }
                CertFreeCertificateContext(cert);
            }
        }
    }
    CryptMsgClose(message); CertCloseStore(store, 0);
    return match;
}
// Downloads the setup and its checksum to a temporary folder and returns the setup's path only if
// every check passes.
inline winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> download(Release release) {
    winrt::Windows::Web::Http::HttpClient client;
    client.DefaultRequestHeaders().UserAgent().ParseAdd(L"EarbudControls");
    auto sums = co_await client.GetStringAsync(winrt::Windows::Foundation::Uri(release.sha_url));
    std::wstring expected{sums.c_str()};
    expected = expected.substr(0, expected.find_first_of(L" \t\r\n"));
    for (auto& c : expected) c = static_cast<wchar_t>(towupper(c));
    if (expected.size() != 64) co_return L"";
    auto buffer = co_await client.GetBufferAsync(winrt::Windows::Foundation::Uri(release.setup_url));
    const auto folder = std::filesystem::temp_directory_path() / L"EarbudControlsUpdate";
    std::error_code ec; std::filesystem::create_directories(folder, ec);
    const auto path = folder / (L"EarbudControls-Setup-" + release.version + L".exe");
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length()); }
    if (file_sha256(path) != expected || !signed_by_us(path)) { std::filesystem::remove(path, ec); co_return L""; }
    co_return winrt::hstring(path.wstring());
}
}
