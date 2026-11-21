// foo_liquidsoap_record.cpp - Liquidsoap Record for foobar2000 (x86 + x64)
//
// Thin wrapper around record.liq: native foobar2000 UI that spawns
//   <liquidsoap exe> <record.liq> -- -url ... -dir ... (same args as the Python UI)
// and stops it through http://127.0.0.1:1234/stop_all (graceful), falling back
// to TerminateProcess after a timeout.

#include <foobar2000/SDK/foobar2000.h>

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

// Optional Columns UI panel: define FOO_RECORD_ENABLE_CUI (vcxproj property EnableCUI=true)
// and make sure the Columns UI SDK is at <SDK root>\columns_ui-sdk\.
#ifdef FOO_RECORD_ENABLE_CUI
#include <columns_ui-sdk/ui_extension.h>
#define FOO_RECORD_HAVE_CUI 1
#else
#define FOO_RECORD_HAVE_CUI 0
#endif

DECLARE_COMPONENT_VERSION(
    "Liquidsoap Record", "1.0.0",
    "Record internet radio streams with Liquidsoap (record.liq).\n"
    "Requires a user-supplied liquidsoap executable and ffmpeg/ffprobe (path set in the window, or in PATH).");
VALIDATE_COMPONENT_FILENAME("foo_liquidsoap_record.dll");

static const GUID guid_wndpos = { 0x6a1f3c52, 0x0b7e, 0x4d28, { 0x9e, 0x41, 0x7c, 0x35, 0xd8, 0x02, 0xaf, 0x19 } };
static const GUID guid_ffmpeg = { 0x3e9b52a7, 0x61c4, 0x4f0a, { 0x8d, 0x27, 0xb5, 0x0e, 0x94, 0x1a, 0x6c, 0xd3 } };
static const GUID guid_ffprobe = { 0x9c2d7e18, 0x4a3b, 0x4e65, { 0xa1, 0x90, 0x2f, 0x78, 0xc3, 0xd5, 0x0b, 0x46 } };
static const GUID guid_exe = { 0xbfd7491c, 0x9cfa, 0x45e3, { 0x82, 0x86, 0x06, 0xd1, 0x14, 0x83, 0x15, 0xbe } };
static const GUID guid_script = { 0x4bfe673c, 0xec76, 0x4ea3, { 0x84, 0xdf, 0xbe, 0xe7, 0x22, 0xec, 0x4b, 0x8c } };
static const GUID guid_url = { 0x7312a512, 0xd15b, 0x4f0e, { 0xb4, 0x26, 0x52, 0x9a, 0x71, 0x08, 0x0c, 0x00 } };
static const GUID guid_station = { 0x2d4ebf1c, 0x39d0, 0x4b96, { 0x91, 0xa9, 0x1c, 0x91, 0xd8, 0x22, 0x7f, 0x0c } };
static const GUID guid_directory = { 0xc0451c79, 0xac8c, 0x4d99, { 0x90, 0x88, 0xd5, 0x66, 0x34, 0x9b, 0x9c, 0x8b } };
static const GUID guid_format = { 0xcbd28fe1, 0xb1c7, 0x4097, { 0x93, 0x01, 0x5b, 0x34, 0xce, 0x66, 0xc9, 0xdd } };
static const GUID guid_codec = { 0xf71b2435, 0x4d55, 0x4b6b, { 0x9c, 0x6f, 0xb4, 0x53, 0x69, 0x01, 0xe0, 0x86 } };
static const GUID guid_bitrate = { 0x139185ee, 0x7318, 0x4caa, { 0xba, 0x1a, 0x78, 0x88, 0x5d, 0xf8, 0x2b, 0x20 } };
static const GUID guid_samplerate = { 0x5efb2632, 0x1498, 0x459e, { 0x81, 0xc2, 0xf0, 0x9d, 0xff, 0xc7, 0xa7, 0x8a } };
static const GUID guid_loglevel = { 0xf16d60df, 0xaa60, 0x40a9, { 0x9a, 0x17, 0x32, 0xdb, 0xea, 0x07, 0xa1, 0x93 } };
static const GUID guid_device = { 0x1a3364d0, 0x0249, 0x455b, { 0x94, 0xb4, 0x27, 0xcb, 0x6b, 0x2d, 0xb8, 0xcc } };
static const GUID guid_timeout = { 0x43459f6f, 0x2813, 0x43d2, { 0x9d, 0x84, 0xa6, 0xb8, 0x66, 0x41, 0xbf, 0x2c } };
static const GUID guid_tunein = { 0x36673ba1, 0x876a, 0x4ea7, { 0x9c, 0xb3, 0x3c, 0x2d, 0x37, 0x08, 0xdc, 0x66 } };
static const GUID guid_host = { 0x71b6d2b6, 0x326b, 0x4132, { 0x93, 0x53, 0x36, 0xf1, 0x79, 0xa4, 0xc2, 0xff } };
static const GUID guid_port = { 0xfc23a24d, 0x06e0, 0x4396, { 0xb1, 0x0f, 0x89, 0x33, 0xe5, 0xbf, 0xe8, 0x1f } };
static const GUID guid_password = { 0xf0113124, 0xaa89, 0x4997, { 0x9c, 0x80, 0x0d, 0x3d, 0xa1, 0x70, 0x3c, 0xdb } };
static const GUID guid_transcode = { 0xd8ea93b5, 0xf80f, 0x480e, { 0xb2, 0x62, 0x42, 0x97, 0x36, 0x19, 0x53, 0x37 } };
static const GUID guid_listen = { 0x69802545, 0xb74a, 0x4636, { 0x82, 0x57, 0xa7, 0x07, 0x82, 0xcc, 0x4c, 0x33 } };
static const GUID guid_keep = { 0x158fdb72, 0xeec9, 0x4803, { 0x97, 0xe9, 0xb5, 0xcf, 0x11, 0x87, 0xfa, 0xef } };
static const GUID guid_m3u = { 0x49537115, 0x2156, 0x4682, { 0xbb, 0xfb, 0xc4, 0xbf, 0x46, 0x83, 0x65, 0x27 } };
static const GUID guid_covers = { 0x6168e70c, 0xab95, 0x420f, { 0xa8, 0x70, 0xf0, 0x4b, 0xe6, 0x1c, 0x34, 0x46 } };
static const GUID guid_single = { 0xb3e7fc43, 0x4e4f, 0x4253, { 0x85, 0x0a, 0xd3, 0xc7, 0x8b, 0x65, 0x8f, 0x2e } };
static const GUID guid_relay = { 0xf0259e8c, 0x9ecd, 0x42f5, { 0x8e, 0xb1, 0xdf, 0x66, 0x08, 0xb8, 0x80, 0x7d } };
static const GUID guid_menu = { 0xbcb8fd72, 0x058c, 0x4afa, { 0x90, 0xcb, 0x20, 0x13, 0xd2, 0x34, 0x9a, 0x87 } };
static const GUID guid_ctx_open = { 0xaaeb5584, 0xff36, 0x40a0, { 0xbf, 0xb4, 0x25, 0xa2, 0xd5, 0xcd, 0x98, 0x2d } };
static const GUID guid_ctx_start = { 0x92a130d5, 0xa25e, 0x4167, { 0xa0, 0xfd, 0xbf, 0x34, 0x86, 0x80, 0x96, 0x30 } };
static const GUID guid_dui = { 0x015d1d74, 0xdd37, 0x4bf2, { 0xa5, 0xeb, 0x98, 0x72, 0xd2, 0x5c, 0xf8, 0x94 } };
static const GUID guid_cui = { 0x7b303f09, 0x947b, 0x4e49, { 0x91, 0xbc, 0x5b, 0x9f, 0x42, 0x6e, 0xc4, 0x44 } };

namespace {

// ============================================================
// SETTINGS (all stored as strings; checkboxes are "0"/"1")
// ============================================================

#define CFGSTR(n, def) static cfg_string c_##n(guid_##n, def)

CFGSTR(exe, "");
static cfg_string c_wndpos(guid_wndpos, ""); // popup window: "left top right bottom maximized"
CFGSTR(script, "");
CFGSTR(ffmpeg, "");
CFGSTR(ffprobe, "");
CFGSTR(url, "");
CFGSTR(station, "");
CFGSTR(directory, "");
CFGSTR(format, "mp3");
CFGSTR(codec, "libmp3lame");
CFGSTR(bitrate, "320k");
CFGSTR(samplerate, "44100");
CFGSTR(loglevel, "2");
CFGSTR(device, "portaudio");
CFGSTR(timeout, "0");
CFGSTR(tunein, "");
CFGSTR(host, "localhost");
CFGSTR(port, "8000");
CFGSTR(password, "hackme");
CFGSTR(transcode, "0");
CFGSTR(listen, "0");
CFGSTR(keep, "0");
CFGSTR(m3u, "0");
CFGSTR(covers, "0");
CFGSTR(single, "0");
CFGSTR(relay, "0");

enum Kind { K_EDIT, K_PASS, K_CHECK, K_COMBO, K_COMBO_LIST, K_FILE_EXE, K_FILE_LIQ, K_DIR };

struct Field {
    const char* key;
    const wchar_t* label;
    Kind kind;
    const wchar_t* choices; // '|' separated
    cfg_string* cfg;
    bool wide;
};

static Field g_fields[] = {
    {"exe", L"Liquidsoap exe", K_FILE_EXE, nullptr, &c_exe, true},
    {"script", L"record.liq", K_FILE_LIQ, nullptr, &c_script, true},
    {"ffmpeg", L"ffmpeg", K_FILE_EXE, nullptr, &c_ffmpeg, true},
    {"ffprobe", L"ffprobe", K_FILE_EXE, nullptr, &c_ffprobe, true},
    {"url", L"Stream URL", K_EDIT, nullptr, &c_url, true},
    {"station", L"Station name", K_EDIT, nullptr, &c_station, true},
    {"directory", L"Directory", K_DIR, nullptr, &c_directory, true},

    {"format", L"Format", K_COMBO_LIST, L"mp3|m4a|flac|ogg|opus", &c_format, false},
    {"codec", L"Codec", K_COMBO, L"libmp3lame|aac|flac|libvorbis|libopus", &c_codec, false},
    {"bitrate", L"Bitrate", K_COMBO, L"96k|128k|192k|256k|320k", &c_bitrate, false},
    {"samplerate", L"Sample rate", K_COMBO, L"44100|48000", &c_samplerate, false},
    {"loglevel", L"Log level", K_COMBO_LIST, L"0|1|2|3|4", &c_loglevel, false},
    {"device", L"Audio device", K_COMBO, L"portaudio|alsa", &c_device, false},
    {"timeout", L"Timeout (s)", K_EDIT, nullptr, &c_timeout, false},
    {"tunein", L"TuneIn ID", K_EDIT, nullptr, &c_tunein, false},
    {"host", L"Relay host", K_EDIT, nullptr, &c_host, false},
    {"port", L"Relay port", K_EDIT, nullptr, &c_port, false},
    {"password", L"Relay password", K_PASS, nullptr, &c_password, false},

    {"transcode", L"Transcode", K_CHECK, nullptr, &c_transcode, false},
    {"listen", L"Listen while recording", K_CHECK, nullptr, &c_listen, false},
    {"keep", L"Keep incomplete recordings", K_CHECK, nullptr, &c_keep, false},
    {"m3u", L"Write M3U", K_CHECK, nullptr, &c_m3u, false},
    {"covers", L"Download covers", K_CHECK, nullptr, &c_covers, false},
    {"single", L"Single track mode", K_CHECK, nullptr, &c_single, false},
    {"relay", L"Icecast relay", K_CHECK, nullptr, &c_relay, false},
};
static const int N_FIELDS = (int)(sizeof(g_fields) / sizeof(g_fields[0]));

static Field* find_field(const char* key) {
    for (auto& f : g_fields)
        if (!strcmp(f.key, key)) return &f;
    return nullptr;
}

// ============================================================
// STRING HELPERS
// ============================================================

static std::wstring W(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring r(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &r[0], n);
    return r;
}

static std::wstring WBytes(const char* s, int len) {
    if (len <= 0) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, &r[0], n);
    return r;
}

static std::string U(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n, nullptr, nullptr);
    return r;
}

static std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    n = GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

// Windows command-line quoting (CommandLineToArgvW rules)
static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += c; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

static std::wstring cfg_w(const char* key) {
    Field* f = find_field(key);
    return f ? W(f->cfg->get_ptr()) : L"";
}

static bool cfg_on(const char* key) {
    Field* f = find_field(key);
    return f && !strcmp(f->cfg->get_ptr(), "1");
}

static std::wstring dir_of(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}

static std::wstring default_script() {
    std::wstring me = W(core_api::get_my_full_path());
    std::wstring d = dir_of(me);
    return d.empty() ? L"record.liq" : d + L"\\record.liq";
}

// ============================================================
// LOG
// ============================================================

static const UINT WM_APP_LOG = WM_APP + 1;
static const UINT WM_APP_STATE = WM_APP + 2;

static std::mutex g_log_mx;
static std::wstring g_log;

// every live view (panel instances + popup) registers its HWND here
static std::mutex g_views_mx;
static std::vector<HWND> g_views;

static const UINT WM_APP_RELOAD = WM_APP + 3;
static const UINT WM_APP_FORMAT = WM_APP + 4;
static const UINT WM_APP_DESELECT = WM_APP + 5;

static std::vector<HWND> views_snapshot() {
    std::lock_guard<std::mutex> lk(g_views_mx);
    return g_views;
}

static void post_log(const std::wstring& line) {
    {
        std::lock_guard<std::mutex> lk(g_log_mx);
        g_log += line;
        g_log += L"\r\n";
        if (g_log.size() > 300000) g_log.erase(0, 100000);
    }
    for (HWND w : views_snapshot()) {
        auto* s = new std::wstring(line);
        if (!PostMessageW(w, WM_APP_LOG, 0, (LPARAM)s)) delete s;
    }
}

static void post_state() {
    for (HWND w : views_snapshot()) PostMessageW(w, WM_APP_STATE, 0, 0);
}

// ask every view except `except` to re-read the shared settings
static void reload_all(HWND except) {
    for (HWND w : views_snapshot())
        if (w != except) PostMessageW(w, WM_APP_RELOAD, 0, 0);
}

// ============================================================
// RUNNER
// ============================================================

static std::mutex g_proc_mx;
static HANDLE g_proc = nullptr;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_stopping{false};
static std::thread g_reader;
static DWORD g_start_tick = 0;
static std::wstring g_current = L"-";

static void handle_line(const std::wstring& line) {
    post_log(line);
    std::wstring low = line;
    for (auto& c : low) c = (wchar_t)towlower(c);
    size_t p = low.find(L"recording..");
    if (p != std::wstring::npos) {
        std::wstring v = trim(line.substr(p + 11));
        if (!v.empty()) {
            std::lock_guard<std::mutex> lk(g_log_mx);
            g_current = v;
        }
    }
}

static void reader_thread(HANDLE rd, HANDLE proc) {
    std::string acc;
    char buf[4096];
    auto feed = [&](const char* d, DWORD n) {
        for (DWORD i = 0; i < n; ++i) {
            if (d[i] == '\n') {
                if (!acc.empty() && acc.back() == '\r') acc.pop_back();
                handle_line(WBytes(acc.data(), (int)acc.size()));
                acc.clear();
            } else {
                acc.push_back(d[i]);
            }
        }
    };
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail) {
            DWORD n = 0;
            DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!ReadFile(rd, buf, want, &n, nullptr) || !n) break;
            feed(buf, n);
            continue;
        }
        if (WaitForSingleObject(proc, 100) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) continue;
            break;
        }
    }
    if (!acc.empty()) handle_line(WBytes(acc.data(), (int)acc.size()));
    CloseHandle(rd);

    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    post_log(L"[GUI] Liquidsoap exited with code " + std::to_wstring(code) + L".");
    {
        std::lock_guard<std::mutex> lk(g_proc_mx);
        CloseHandle(proc);
        g_proc = nullptr;
    }
    g_running = false;
    post_state();
}

// Builds the full command line. mask=true hides the relay password (for the log).
static bool build_cmd(std::wstring& cmd, std::wstring& script, std::wstring& err, bool mask) {
    std::wstring url = trim(cfg_w("url"));
    std::wstring dir = trim(cfg_w("directory"));
    if (url.empty()) { err = L"Stream URL is required."; return false; }
    if (dir.empty()) { err = L"Recording directory is required."; return false; }

    std::wstring exe = trim(cfg_w("exe"));
    if (exe.empty()) exe = L"liquidsoap"; // resolved through PATH
    script = trim(cfg_w("script"));
    if (script.empty()) script = default_script();
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"record.liq not found: " + script;
        return false;
    }

    auto orSpace = [](std::wstring s) { s = trim(s); return s.empty() ? std::wstring(L" ") : s; };
    auto orZero = [](std::wstring s) { s = trim(s); return s.empty() ? std::wstring(L"0") : s; };
    auto flag = [](const char* k) { return std::wstring(cfg_on(k) ? L"1" : L"0"); };

    std::vector<std::wstring> a = {
        L"-url", url,
        L"-dir", dir,
        L"-station", orSpace(cfg_w("station")),
        L"-transcode", flag("transcode"),
        L"-samplerate", trim(cfg_w("samplerate")),
        L"-format", trim(cfg_w("format")),
        L"-codec", trim(cfg_w("codec")),
        L"-bitrate", trim(cfg_w("bitrate")),
        L"-listen", flag("listen"),
        L"-device", trim(cfg_w("device")),
        L"-log", trim(cfg_w("loglevel")),
        L"-keep", flag("keep"),
        L"-id", orSpace(cfg_w("tunein")),
        L"-relay", flag("relay"),
        L"-host", trim(cfg_w("host")),
        L"-port", trim(cfg_w("port")),
        L"-password", mask ? std::wstring(L"********") : cfg_w("password"),
        L"-m3u", flag("m3u"),
        L"-covers", flag("covers"),
        L"-single", flag("single"),
        L"-timeout", orZero(cfg_w("timeout")),
    };

    cmd = quote_arg(exe) + L" " + quote_arg(script) + L" --";
    for (auto& s : a) cmd += L" " + quote_arg(s);
    return true;
}

// Environment for the recorder: record.liq runs plain "ffmpeg" / "ffprobe", so a configured
// location is put in front of PATH. Empty boxes leave PATH untouched.
static std::vector<wchar_t> make_child_env(std::wstring& note) {
    std::vector<std::wstring> dirs;
    static const char* const keys[2] = {"ffmpeg", "ffprobe"};
    static const wchar_t* const names[2] = {L"ffmpeg", L"ffprobe"};
    for (int ki = 0; ki < 2; ++ki) {
        const wchar_t* name = names[ki];
        std::wstring p = trim(cfg_w(keys[ki]));
        if (p.empty()) continue;
        DWORD at = GetFileAttributesW(p.c_str());
        std::wstring d = p;
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
            d = dir_of(p);
            std::wstring base = p.substr(d.size());
            while (!base.empty() && (base[0] == L'\\' || base[0] == L'/')) base.erase(0, 1);
            std::wstring want = std::wstring(name) + L".exe";
            if (_wcsicmp(base.c_str(), want.c_str()) != 0 && _wcsicmp(base.c_str(), name) != 0)
                note += L"[GUI] Warning: the " + std::wstring(name) + L" file is named \"" + base +
                        L"\" but record.liq runs \"" + name + L"\"; name it " + want + L".\n";
        }
        if (d.empty()) continue;
        bool dup = false;
        for (auto& e : dirs) if (!_wcsicmp(e.c_str(), d.c_str())) dup = true;
        if (!dup) dirs.push_back(d);
    }
    std::wstring prefix;
    for (auto& d : dirs) prefix += d + L";";

    std::vector<wchar_t> out;
    auto add = [&](const std::wstring& e) { out.insert(out.end(), e.begin(), e.end()); out.push_back(0); };
    bool havePath = false;
    if (LPWCH env = GetEnvironmentStringsW()) {
        for (const wchar_t* e = env; *e; e += wcslen(e) + 1) {
            std::wstring ent = e;
            if (!prefix.empty() && ent.size() >= 5 && !_wcsnicmp(ent.c_str(), L"PATH=", 5)) {
                havePath = true;
                ent = ent.substr(0, 5) + prefix + ent.substr(5);
            }
            add(ent);
        }
        FreeEnvironmentStringsW(env);
    }
    if (!prefix.empty() && !havePath) add(L"PATH=" + prefix);
    out.push_back(0);
    if (!prefix.empty()) note += L"[GUI] ffmpeg/ffprobe folder(s) added to PATH: " + prefix.substr(0, prefix.size() - 1) + L"\n";
    return out;
}

static bool start_recording() {
    if (g_running) { post_log(L"[GUI] Already recording."); return false; }
    if (g_reader.joinable()) g_reader.join();

    std::wstring cmd, script, err;
    if (!build_cmd(cmd, script, err, false)) { post_log(L"[ERROR] " + err); return false; }
    std::wstring shown, s2, e2;
    build_cmd(shown, s2, e2, true);

    post_log(std::wstring(100, L'='));
    post_log(L"LIQUIDSOAP RECORD");
    post_log(L"Command:");
    post_log(shown);
    post_log(std::wstring(100, L'='));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { post_log(L"[ERROR] CreatePipe failed."); return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    std::wstring wd = dir_of(script);
    std::wstring envnote;
    std::vector<wchar_t> envblock = make_child_env(envnote);
    while (!envnote.empty()) {
        size_t e = envnote.find(L'\n');
        post_log(envnote.substr(0, e));
        if (e == std::wstring::npos) break;
        envnote.erase(0, e + 1);
    }

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                             envblock.data(), wd.empty() ? nullptr : wd.c_str(), &si, &pi);
    DWORD le = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        post_log(L"[ERROR] Could not start Liquidsoap (error " + std::to_wstring(le) +
                 L"). Set the liquidsoap executable path or add it to PATH.");
        return false;
    }
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lk(g_proc_mx);
        g_proc = pi.hProcess;
    }
    {
        std::lock_guard<std::mutex> lk(g_log_mx);
        g_current = L"-";
    }
    g_start_tick = GetTickCount();
    g_stopping = false;
    g_running = true;
    g_reader = std::thread(reader_thread, rd, pi.hProcess);
    post_state();
    return true;
}

static bool http_stop_all() {
    bool good = false;
    HINTERNET ses = WinHttpOpen(L"foo_liquidsoap_record", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", 1234, 0);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(con, L"GET", L"/stop_all", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX))
                    good = (status == 200);
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return good;
}

// Blocking. Caller must have set g_stopping = true.
static void stop_worker() {
    HANDLE dup = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_proc_mx);
        if (g_proc)
            DuplicateHandle(GetCurrentProcess(), g_proc, GetCurrentProcess(), &dup, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
    }
    if (!dup) { g_stopping = false; post_state(); return; }
    post_state();

    post_log(L"[GUI] Sending Liquidsoap 'stop_all' command to 127.0.0.1:1234...");
    if (http_stop_all())
        post_log(L"[GUI] 'stop_all' command sent.");
    else
        post_log(L"[GUI] Could not send 'stop_all'.");

    if (WaitForSingleObject(dup, 15000) != WAIT_OBJECT_0) {
        post_log(L"[GUI] Graceful shutdown timed out after 15 seconds, terminating.");
        TerminateProcess(dup, 1);
        WaitForSingleObject(dup, 3000);
    }
    CloseHandle(dup);
    g_stopping = false;
    post_state();
}

static void stop_recording_async() {
    if (!g_running) return;
    if (g_stopping.exchange(true)) return;
    std::thread(stop_worker).detach();
}

static void shutdown_sync() {
    if (g_running) {
        if (!g_stopping.exchange(true)) stop_worker();
        while (g_stopping) Sleep(50);
    }
    if (g_reader.joinable()) g_reader.join();
}

// ============================================================
// VIEW  (one per DUI element / CUI panel / popup window)
// All views share the same settings and the same running recorder.
// ============================================================

enum {
    ID_FIELD = 1000,   // + field index
    ID_BROWSE = 2000,  // + field index
    ID_START = 3001,
    ID_STOP = 3002,
    ID_COPY = 3003,
    ID_LOG = 3004,
    ID_STATUS = 3005,
};

static_assert(N_FIELDS <= 32, "raise View array sizes");

struct View {
    HWND wnd = nullptr;
    bool popup = false;
    int scroll = 0;
    bool in_layout = false;
    HWND lbl[32] = {};
    HWND ctl[32] = {};
    HWND btn[32] = {};
    HWND start = nullptr, stop = nullptr, copy = nullptr, status = nullptr;
    HWND log_lbl = nullptr, log = nullptr;
    // theme
    bool dark = false;
    COLORREF bg = 0, fg = 0, ebg = 0;
    HBRUSH bg_brush = nullptr, edit_brush = nullptr;
};

static HFONT g_font = nullptr;
static HWND g_popup = nullptr; // main thread only
static const wchar_t* WND_CLASS = L"foo_liquidsoap_record_view";

static View* V(HWND h) { return (View*)GetWindowLongPtrW(h, GWLP_USERDATA); }

static bool is_combo(Kind k) { return k == K_COMBO || k == K_COMBO_LIST; }

// ---- popup window position / size -------------------------------------------------
static void save_popup_placement(HWND h) {
    if (!h || !IsWindow(h)) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(h, &wp)) return;
    char buf[96];
    sprintf_s(buf, "%ld %ld %ld %ld %d", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
              wp.rcNormalPosition.right, wp.rcNormalPosition.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    c_wndpos.set_string(buf);
}

// Saved rectangle (restored size) if there is one and it is still on a monitor.
static bool load_popup_placement(RECT& r, bool& maximized) {
    long l, t, rr, b;
    int m = 0;
    if (sscanf_s(c_wndpos.get_ptr(), "%ld %ld %ld %ld %d", &l, &t, &rr, &b, &m) < 4) return false;
    r = {l, t, rr, b};
    if (r.right - r.left < 300 || r.bottom - r.top < 200) return false;
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false; // monitor is gone
    maximized = m != 0;
    return true;
}

// Clear the text selection of an editable combo. CB_SETEDITSEL with start=-1 packed into a
// WORD can be read as "select everything", so address the inner edit control directly.
static void clear_combo_sel(HWND h) {
    if (!h || !IsWindow(h)) return;
    COMBOBOXINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetComboBoxInfo(h, &ci) && ci.hwndItem) SendMessageW(ci.hwndItem, EM_SETSEL, 0, 0);
    else SendMessageW(h, CB_SETEDITSEL, 0, MAKELPARAM(0, 0));
}

// Clear a leftover selection in every combo that does not have the focus.
static void sweep_combo_sel(View* v) {
    HWND foc = GetFocus();
    for (int i = 0; i < N_FIELDS; ++i) {
        HWND h = v->ctl[i];
        if (!h || !is_combo(g_fields[i].kind)) continue;
        COMBOBOXINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetComboBoxInfo(h, &ci) || !ci.hwndItem) continue;
        if (foc == h || foc == ci.hwndItem) continue;
        DWORD a = 0, b = 0;
        SendMessageW(ci.hwndItem, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if (a != b) SendMessageW(ci.hwndItem, EM_SETSEL, 0, 0);
    }
}

// "List" combos are CBS_DROPDOWN with a read-only edit (a CBS_DROPDOWNLIST is drawn grey by
// Windows). Clicking the edit part must open the list like a normal drop-down list does.
static LRESULT CALLBACK ro_combo_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR combo) {
    HWND cb = (HWND)combo;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        SendMessageW(cb, CB_SHOWDROPDOWN, SendMessageW(cb, CB_GETDROPPEDSTATE, 0, 0) ? FALSE : TRUE, 0);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_SETFOCUS: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        HideCaret(h);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, ro_combo_edit_proc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void make_list_combo_readonly(HWND cb) {
    COMBOBOXINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetComboBoxInfo(cb, &ci) || !ci.hwndItem) return;
    SendMessageW(ci.hwndItem, EM_SETREADONLY, TRUE, 0);
    SetWindowSubclass(ci.hwndItem, ro_combo_edit_proc, 1, (DWORD_PTR)cb);
}

// Is this the combo itself or the edit inside one? (colour messages arrive with either)
static bool is_combo_or_its_edit(HWND h) {
    wchar_t c[32] = L"";
    GetClassNameW(h, c, 32);
    if (!_wcsicmp(c, L"ComboBox")) return true;
    if (_wcsicmp(c, L"Edit")) return false;
    HWND p = GetParent(h);
    if (!p) return false;
    GetClassNameW(p, c, 32);
    return !_wcsicmp(c, L"ComboBox");
}


static void load_to_controls(View* v) {
    for (int i = 0; i < N_FIELDS; ++i) {
        Field& f = g_fields[i];
        HWND h = v->ctl[i];
        if (!h) continue;
        std::wstring val = W(f.cfg->get_ptr());
        if (f.kind == K_CHECK) {
            int want = (val == L"1") ? BST_CHECKED : BST_UNCHECKED;
            if (SendMessageW(h, BM_GETCHECK, 0, 0) != want) SendMessageW(h, BM_SETCHECK, want, 0);
        } else if (f.kind == K_COMBO_LIST) {
            LRESULT idx = SendMessageW(h, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)val.c_str());
            if (idx == CB_ERR) idx = 0;
            if (SendMessageW(h, CB_GETCURSEL, 0, 0) != idx) SendMessageW(h, CB_SETCURSEL, (WPARAM)idx, 0);
        } else {
            if (get_text(h) != val) {
                SetWindowTextW(h, val.c_str());
                if (f.kind == K_COMBO) clear_combo_sel(h); // no highlight
            }
        }
    }
}

static void save_from_controls(View* v) {
    for (int i = 0; i < N_FIELDS; ++i) {
        Field& f = g_fields[i];
        HWND h = v->ctl[i];
        if (!h) continue;
        if (f.kind == K_CHECK)
            f.cfg->set_string(SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
        else
            f.cfg->set_string(U(trim(get_text(h))).c_str());
    }
}

static void update_status(View* v) {
    if (!v->status) return;
    std::wstring s;
    if (g_stopping) {
        s = L"Stopping...";
    } else if (g_running) {
        DWORD sec = (GetTickCount() - g_start_tick) / 1000;
        wchar_t t[32];
        swprintf(t, 32, L"%02u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60),
                 (unsigned)(sec % 60));
        std::wstring cur;
        {
            std::lock_guard<std::mutex> lk(g_log_mx);
            cur = g_current;
        }
        s = L"Recording  " + std::wstring(t) + L"   File: " + cur;
    } else {
        s = L"Idle";
    }
    if (get_text(v->status) != s) SetWindowTextW(v->status, s.c_str());
    EnableWindow(v->start, !g_running);
    EnableWindow(v->stop, g_running && !g_stopping);
}

static void append_log(View* v, const std::wstring& line) {
    if (!v->log) return;
    if (GetWindowTextLengthW(v->log) > 250000) {
        SendMessageW(v->log, EM_SETSEL, 0, 100000);
        SendMessageW(v->log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    SendMessageW(v->log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    std::wstring t = line + L"\r\n";
    SendMessageW(v->log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
    SendMessageW(v->log, EM_SCROLLCARET, 0, 0);
}

static void browse(View* v, int idx) {
    Field& f = g_fields[idx];
    HWND h = v->ctl[idx];
    if (f.kind == K_DIR) {
        BROWSEINFOW bi{};
        bi.hwndOwner = v->wnd;
        bi.lpszTitle = L"Select recording directory";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH * 2] = {};
            if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(h, path);
            CoTaskMemFree(pidl);
        }
        return;
    }
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = v->wnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (f.kind == K_FILE_EXE) {
        ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files\0*.*\0";
        ofn.lpstrTitle = !strcmp(f.key, "ffmpeg")    ? L"Select ffmpeg executable"
                         : !strcmp(f.key, "ffprobe") ? L"Select ffprobe executable"
                                                     : L"Select liquidsoap executable";
    } else {
        ofn.lpstrFilter = L"Liquidsoap scripts (*.liq)\0*.liq\0All files\0*.*\0";
        ofn.lpstrTitle = L"Select record.liq";
    }
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(h, file);
}

static void copy_command(View* v) {
    save_from_controls(v);
    std::wstring cmd, script, err;
    if (!build_cmd(cmd, script, err, false)) { post_log(L"[ERROR] " + err); return; }
    if (!OpenClipboard(v->wnd)) return;
    EmptyClipboard();
    size_t bytes = (cmd.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        memcpy(GlobalLock(g), cmd.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
    post_log(L"[GUI] Command copied to clipboard.");
}

static int field_index(const char* key) {
    for (int i = 0; i < N_FIELDS; ++i)
        if (!strcmp(g_fields[i].key, key)) return i;
    return -1;
}

static void on_format_changed(View* v) {
    int fi = field_index("format"), ci = field_index("codec");
    if (fi < 0 || ci < 0 || !v->ctl[fi] || !v->ctl[ci]) return;
    std::wstring fmt = get_text(v->ctl[fi]);
    const wchar_t* codec = nullptr;
    if (fmt == L"mp3") codec = L"libmp3lame";
    else if (fmt == L"m4a") codec = L"aac";
    else if (fmt == L"flac") codec = L"flac";
    else if (fmt == L"ogg") codec = L"libvorbis";
    else if (fmt == L"opus") codec = L"libopus";
    if (codec) {
        SetWindowTextW(v->ctl[ci], codec);
        clear_combo_sel(v->ctl[ci]);
    }
}

static BOOL CALLBACK set_font_proc(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

static void build_controls(View* v) {
    HINSTANCE inst = core_api::get_my_instance();
    auto mk = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD st, int id) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, v->wnd,
                               (HMENU)(INT_PTR)id, inst, nullptr);
    };
    for (int i = 0; i < N_FIELDS; ++i) {
        Field& f = g_fields[i];
        int id = ID_FIELD + i;
        if (f.kind == K_CHECK) {
            v->ctl[i] = mk(0, L"BUTTON", f.label, BS_AUTOCHECKBOX | WS_TABSTOP, id);
            continue;
        }
        v->lbl[i] = mk(0, L"STATIC", f.label, SS_LEFT, -1);
        if (is_combo(f.kind)) {
            DWORD cs = CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL |
                       WS_TABSTOP;
            v->ctl[i] = mk(0, L"COMBOBOX", L"", cs, id);
            std::wstring ch = f.choices ? f.choices : L"";
            size_t pos = 0;
            while (pos <= ch.size()) {
                size_t e = ch.find(L'|', pos);
                if (e == std::wstring::npos) e = ch.size();
                if (e > pos) SendMessageW(v->ctl[i], CB_ADDSTRING, 0, (LPARAM)ch.substr(pos, e - pos).c_str());
                pos = e + 1;
            }
            if (f.kind == K_COMBO_LIST) make_list_combo_readonly(v->ctl[i]);
        } else {
            DWORD es = ES_AUTOHSCROLL | WS_TABSTOP | (f.kind == K_PASS ? ES_PASSWORD : 0);
            v->ctl[i] = mk(WS_EX_CLIENTEDGE, L"EDIT", L"", es, id);
        }
        if (f.kind == K_FILE_EXE || f.kind == K_FILE_LIQ || f.kind == K_DIR)
            v->btn[i] = mk(0, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_BROWSE + i);
    }
    v->start = mk(0, L"BUTTON", L"Start", BS_PUSHBUTTON | WS_TABSTOP, ID_START);
    v->stop = mk(0, L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, ID_STOP);
    v->copy = mk(0, L"BUTTON", L"Copy command", BS_PUSHBUTTON | WS_TABSTOP, ID_COPY);
    v->status = mk(0, L"STATIC", L"Idle", SS_LEFT | SS_ENDELLIPSIS, ID_STATUS);
    v->log_lbl = mk(0, L"STATIC", L"Log", SS_LEFT, -1);
    v->log = mk(WS_EX_CLIENTEDGE, L"EDIT", L"",
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, ID_LOG);
    SendMessageW(v->log, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    EnumChildWindows(v->wnd, set_font_proc, (LPARAM)g_font);
}

// Responsive layout: 1 or 2 columns by width, log fills the rest, vertical scroll if too small.
static void layout(View* v) {
    if (v->in_layout) return;
    v->in_layout = true;
    for (int pass = 0; pass < 2; ++pass) {
        RECT rc;
        GetClientRect(v->wnd, &rc);
        int Wd = rc.right, H = rc.bottom;
        if (Wd < 60 || H < 60) break;

        const int M = 10, ROW = 28, LABEL_W = 100, BTN_W = 76;
        int ncols = Wd >= 560 ? 2 : 1;
        int colW = (Wd - (ncols + 1) * M) / ncols;

        HDWP dwp = BeginDeferWindowPos(128);
        auto put = [&](HWND h, int x, int yy, int w, int hh) {
            if (h && dwp)
                dwp = DeferWindowPos(dwp, h, nullptr, x, yy - v->scroll, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        auto place = [&](int i, int x, int y, int w) {
            Field& f = g_fields[i];
            if (f.kind == K_CHECK) { put(v->ctl[i], x, y, w, 22); return; }
            put(v->lbl[i], x, y + 4, LABEL_W - 5, 20);
            int cx = x + LABEL_W, cw = w - LABEL_W;
            if (v->btn[i]) cw -= BTN_W + 6;
            if (cw < 40) cw = 40;
            put(v->ctl[i], cx, y, cw, is_combo(f.kind) ? 200 : 22);
            if (v->btn[i]) put(v->btn[i], cx + cw + 6, y - 1, BTN_W, 24);
        };

        int yy = M;
        for (int i = 0; i < N_FIELDS; ++i)
            if (g_fields[i].wide) { place(i, M, yy, Wd - 2 * M); yy += ROW; }

        int col = 0;
        for (int i = 0; i < N_FIELDS; ++i) {
            if (g_fields[i].wide || g_fields[i].kind == K_CHECK) continue;
            place(i, M + col * (colW + M), yy, colW);
            if (++col == ncols) { col = 0; yy += ROW; }
        }
        if (col) yy += ROW;

        col = 0;
        for (int i = 0; i < N_FIELDS; ++i) {
            if (g_fields[i].kind != K_CHECK) continue;
            place(i, M + col * (colW + M), yy, colW);
            if (++col == ncols) { col = 0; yy += 24; }
        }
        if (col) yy += 24;
        yy += 8;

        put(v->start, M, yy, 90, 26);
        put(v->stop, M + 96, yy, 90, 26);
        put(v->copy, M + 192, yy, 110, 26);
        int sw = Wd - M - (M + 312);
        put(v->status, M + 312, yy + 5, sw < 40 ? 40 : sw, 20);
        yy += 32;

        put(v->log_lbl, M, yy, 100, 20);
        yy += 22;
        int logH = H - yy - M;
        if (logH < 120) logH = 120;
        put(v->log, M, yy, Wd - 2 * M, logH);
        int total = yy + logH + M;

        if (dwp) EndDeferWindowPos(dwp);

        int maxpos = total - H;
        if (maxpos < 0) maxpos = 0;
        int ns = v->scroll;
        if (ns > maxpos) ns = maxpos;
        if (ns < 0) ns = 0;
        bool changed = (ns != v->scroll);
        v->scroll = ns;

        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = total - 1;
        si.nPage = (UINT)H;
        si.nPos = ns;
        SetScrollInfo(v->wnd, SB_VERT, &si, TRUE);

        RECT rc2;
        GetClientRect(v->wnd, &rc2);
        if (rc2.right == Wd && !changed) break; // scrollbar appearing changes width: lay out again
    }
    v->in_layout = false;
}


// ---- theme / dark mode -------------------------------------------------------------
// The host's background colour is sampled by asking the parent window to erase its background
// into a small off-screen bitmap (works for DUI and CUI without any SDK colour API).
// Dark = dark background; text colour is then chosen for contrast.
static bool sample_host_background(HWND parent, COLORREF& out) {
    if (!parent) return false;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, 16, 16);
    ReleaseDC(nullptr, sdc);
    if (!mdc || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mdc) DeleteDC(mdc);
        return false;
    }
    HGDIOBJ old = SelectObject(mdc, bmp);
    const COLORREF sentinel = RGB(1, 2, 3);
    HBRUSH sb = CreateSolidBrush(sentinel);
    RECT r = {0, 0, 16, 16};
    FillRect(mdc, &r, sb);
    DeleteObject(sb);
    LRESULT handled = SendMessageW(parent, WM_ERASEBKGND, (WPARAM)mdc, 0);
    COLORREF px = GetPixel(mdc, 8, 8);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    if (!handled || px == CLR_INVALID || px == sentinel) return false;
    out = px;
    return true;
}

static int luminance(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

static COLORREF lighten(COLORREF c, int pct) {
    auto f = [&](int x) { return x + (255 - x) * pct / 100; };
    return RGB(f(GetRValue(c)), f(GetGValue(c)), f(GetBValue(c)));
}

static bool query_host_colors(View* v, COLORREF& bg, COLORREF& fg) {
    HWND parent = v->popup ? core_api::get_main_window() : GetParent(v->wnd);
    if (!sample_host_background(parent, bg)) return false;
    fg = luminance(bg) < 128 ? RGB(240, 240, 240) : (COLORREF)GetSysColor(COLOR_WINDOWTEXT);
    return true;
}

static void set_theme(HWND h, bool dark, const wchar_t* name) {
    if (!h) return;
    if (!dark) SetWindowTheme(h, nullptr, nullptr);           // back to the default look
    else if (*name) SetWindowTheme(h, name, nullptr);          // DarkMode_* visual style
    else SetWindowTheme(h, L"", L"");                          // classic (checkbox text honours our colour)
}

static void apply_theme(View* v) {
    COLORREF bg = GetSysColor(COLOR_BTNFACE), fg = GetSysColor(COLOR_BTNTEXT);
    COLORREF hb = 0, hf = 0;
    if (query_host_colors(v, hb, hf)) { bg = hb; fg = hf; }
    bool dark = luminance(bg) < 128;

    v->bg = bg;
    v->fg = fg;
    v->dark = dark;
    v->ebg = dark ? lighten(bg, 12) : (COLORREF)GetSysColor(COLOR_WINDOW);
    if (v->bg_brush) DeleteObject(v->bg_brush);
    if (v->edit_brush) DeleteObject(v->edit_brush);
    v->bg_brush = CreateSolidBrush(bg);
    v->edit_brush = CreateSolidBrush(v->ebg);

    set_theme(v->wnd, dark, L"DarkMode_Explorer"); // scrollbars of the view
    for (int i = 0; i < N_FIELDS; ++i) {
        Kind k = g_fields[i].kind;
        if (k == K_CHECK) set_theme(v->ctl[i], dark, L"");
        else {
            set_theme(v->ctl[i], dark, L"DarkMode_CFD");
        }
        set_theme(v->btn[i], dark, L"DarkMode_Explorer");
    }
    set_theme(v->start, dark, L"DarkMode_Explorer");
    set_theme(v->stop, dark, L"DarkMode_Explorer");
    set_theme(v->copy, dark, L"DarkMode_Explorer");
    set_theme(v->log, dark, L"DarkMode_Explorer");

    if (v->popup) {
        BOOL d = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(v->wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof(d));
    }
    RedrawWindow(v->wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void refresh_theme_if_changed(View* v) {
    COLORREF b = 0, f = 0;
    if (!query_host_colors(v, b, f)) return;
    if (b != v->bg || f != v->fg) apply_theme(v);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    View* v = (msg == WM_CREATE || msg == WM_NCCREATE) ? nullptr : V(hwnd);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        View* nv = new View();
        nv->wnd = hwnd;
        nv->popup = (cs->lpCreateParams != nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nv);
        build_controls(nv);
        load_to_controls(nv);
        apply_theme(nv);
        for (int i = 0; i < N_FIELDS; ++i)
            if (is_combo(g_fields[i].kind)) PostMessageW(hwnd, WM_APP_DESELECT, 0, (LPARAM)nv->ctl[i]);
        {
            std::lock_guard<std::mutex> lk(g_log_mx);
            SetWindowTextW(nv->log, g_log.c_str());
        }
        SendMessageW(nv->log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageW(nv->log, EM_SCROLLCARET, 0, 0);
        {
            std::lock_guard<std::mutex> lk(g_views_mx);
            g_views.push_back(hwnd);
        }
        SetTimer(hwnd, 1, 1000, nullptr);
        update_status(nv);
        return 0;
    }
    case WM_ERASEBKGND:
        if (v && v->bg_brush) {
            RECT r;
            GetClientRect(hwnd, &r);
            FillRect((HDC)wp, &r, v->bg_brush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (v && v->bg_brush) {
            HDC dc = (HDC)wp;
            if (msg == WM_CTLCOLORSTATIC && is_combo_or_its_edit((HWND)lp)) {
                if (v->dark) {
                    SetTextColor(dc, v->fg);
                    SetBkColor(dc, v->ebg);
                    return (LRESULT)v->edit_brush;
                }
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
            if ((HWND)lp == v->log) { // read-only log edit
                if (!v->dark) break;
                SetTextColor(dc, v->fg);
                SetBkColor(dc, v->ebg);
                return (LRESULT)v->edit_brush;
            }
            SetTextColor(dc, v->fg);
            SetBkColor(dc, v->bg);
            return (LRESULT)v->bg_brush;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (v && v->dark) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, v->fg);
            SetBkColor(dc, v->ebg);
            return (LRESULT)v->edit_brush;
        }
        break;
    case WM_SIZE:
        if (v) {
            layout(v);
            sweep_combo_sel(v);
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (v && v->popup) save_popup_placement(hwnd);
        break;
    case WM_SHOWWINDOW:
        if (v && wp)
            for (int i = 0; i < N_FIELDS; ++i)
                if (is_combo(g_fields[i].kind) && v->ctl[i] && GetFocus() != v->ctl[i])
                    PostMessageW(hwnd, WM_APP_DESELECT, 0, (LPARAM)v->ctl[i]);
        break;
    case WM_VSCROLL: {
        if (!v) break;
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int p = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: p -= 24; break;
        case SB_LINEDOWN: p += 24; break;
        case SB_PAGEUP: p -= (int)si.nPage; break;
        case SB_PAGEDOWN: p += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: p = si.nTrackPos; break;
        case SB_TOP: p = 0; break;
        case SB_BOTTOM: p = si.nMax; break;
        }
        v->scroll = p;
        layout(v);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!v) break;
        int d = (short)HIWORD(wp);
        v->scroll -= (d / 120) * 48;
        layout(v);
        return 0;
    }
    case WM_COMMAND: {
        if (!v) break;
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == ID_START && code == BN_CLICKED) {
            save_from_controls(v);
            reload_all(hwnd);
            start_recording();
            return 0;
        }
        if (id == ID_STOP && code == BN_CLICKED) { stop_recording_async(); return 0; }
        if (id == ID_COPY && code == BN_CLICKED) { copy_command(v); return 0; }
        if (id >= ID_BROWSE && id < ID_BROWSE + N_FIELDS && code == BN_CLICKED) {
            int i = id - ID_BROWSE;
            browse(v, i);
            save_from_controls(v);
            reload_all(hwnd);
            return 0;
        }
        if (id >= ID_FIELD && id < ID_FIELD + N_FIELDS) {
            int i = id - ID_FIELD;
            Kind k = g_fields[i].kind;
            if (k == K_COMBO_LIST && code == CBN_SELCHANGE && !strcmp(g_fields[i].key, "format")) {
                // the new selection text is only valid after this message is processed
                PostMessageW(hwnd, WM_APP_FORMAT, 0, 0);
                return 0;
            }
            if (k == K_COMBO && code == CBN_SETFOCUS) {
                // the combo selects its text right after taking focus; clear that afterwards
                PostMessageW(hwnd, WM_APP_DESELECT, 0, (LPARAM)v->ctl[i]);
                return 0;
            }
            bool commit = (k == K_CHECK && code == BN_CLICKED) || (is_combo(k) && code == CBN_KILLFOCUS) ||
                          ((k == K_EDIT || k == K_PASS || k == K_FILE_EXE || k == K_FILE_LIQ || k == K_DIR) &&
                           code == EN_KILLFOCUS);
            if (commit) {
                if (is_combo(k)) clear_combo_sel(v->ctl[i]);
                save_from_controls(v);
                reload_all(hwnd);
            }
            return 0;
        }
        break;
    }
    case WM_APP_FORMAT:
        if (v) {
            on_format_changed(v);
            save_from_controls(v);
            reload_all(hwnd);
        }
        return 0;
    case WM_APP_DESELECT:
        clear_combo_sel((HWND)lp);
        return 0;
    case WM_APP_RELOAD:
        if (v) load_to_controls(v);
        return 0;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        if (v) append_log(v, *s);
        return 0;
    }
    case WM_APP_STATE:
        if (v) update_status(v);
        return 0;
    case WM_TIMER:
        if (v) {
            update_status(v);
            sweep_combo_sel(v);
            refresh_theme_if_changed(v);
        }
        return 0;
    case WM_CLOSE:
        if (v && v->popup) {
            save_from_controls(v);
            save_popup_placement(hwnd);
            ShowWindow(hwnd, SW_HIDE); // keep window (and recording) alive
            return 0;
        }
        break;
    case WM_DESTROY: {
        KillTimer(hwnd, 1);
        {
            std::lock_guard<std::mutex> lk(g_views_mx);
            for (size_t i = 0; i < g_views.size(); ++i)
                if (g_views[i] == hwnd) { g_views.erase(g_views.begin() + (ptrdiff_t)i); break; }
        }
        if (v && v->popup) {
            modeless_dialog_manager::g_remove(hwnd);
            if (hwnd == g_popup) g_popup = nullptr;
        }
        return 0;
    }
    case WM_NCDESTROY:
        if (v) {
            if (v->bg_brush) DeleteObject(v->bg_brush);
            if (v->edit_brush) DeleteObject(v->edit_brush);
        }
        delete v;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_class_and_font() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = WND_CLASS;
        RegisterClassW(&wc);
        registered = true;
    }
    if (!g_font) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
}

// Child view for DUI / CUI hosts.
static HWND view_create_child(HWND parent) {
    ensure_class_and_font();
    return CreateWindowExW(WS_EX_CONTROLPARENT, WND_CLASS, L"",
                           WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 100,
                           100, parent, nullptr, core_api::get_my_instance(), nullptr);
}

// Floating window (menu / context menu when no panel is visible).
static void ensure_popup() {
    if (g_popup && IsWindow(g_popup)) {
        ShowWindow(g_popup, SW_SHOW);
        if (IsIconic(g_popup)) ShowWindow(g_popup, SW_RESTORE);
        SetForegroundWindow(g_popup);
        return;
    }
    ensure_class_and_font();
    RECT pr{};
    bool pmax = false;
    bool have_pos = load_popup_placement(pr, pmax);
    g_popup = CreateWindowExW(WS_EX_CONTROLPARENT, WND_CLASS, L"Liquidsoap Record",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                              have_pos ? pr.left : CW_USEDEFAULT, have_pos ? pr.top : CW_USEDEFAULT,
                              have_pos ? pr.right - pr.left : 740, have_pos ? pr.bottom - pr.top : 800,
                              core_api::get_main_window(), nullptr, core_api::get_my_instance(),
                              (LPVOID)(INT_PTR)1);
    if (!g_popup) return;
    {   // no icon in the title bar
        WTA_OPTIONS o{};
        o.dwFlags = WTNCA_NODRAWICON;
        o.dwMask = WTNCA_NODRAWICON;
        SetWindowThemeAttribute(g_popup, WTA_NONCLIENT, &o, sizeof(o));
    }
    modeless_dialog_manager::g_add(g_popup);
    ShowWindow(g_popup, (have_pos && pmax) ? SW_SHOWMAXIMIZED : SW_SHOW);
}

// Open the popup only if no panel is currently visible.
static void show_ui() {
    for (HWND w : views_snapshot())
        if (IsWindowVisible(w)) return;
    ensure_popup();
}


// ---- themed Yes/No box (a MessageBox cannot follow the host's dark / light colours) -----
struct ConfirmState {
    COLORREF bg = 0, fg = 0;
    bool dark = false;
    HBRUSH brush = nullptr;
    bool done = false, yes = false;
};

static LRESULT CALLBACK confirm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ConfirmState* st = (ConfirmState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND:
        if (st && st->brush) {
            RECT r;
            GetClientRect(hwnd, &r);
            FillRect((HDC)wp, &r, st->brush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (st && st->brush) {
            SetTextColor((HDC)wp, st->fg);
            SetBkColor((HDC)wp, st->bg);
            return (LRESULT)st->brush;
        }
        break;
    case DM_GETDEFID:
        return MAKELONG(IDNO, DC_HASDEFID);
    case WM_COMMAND:
        if (st) {
            switch (LOWORD(wp)) {
            case IDYES: st->yes = true; st->done = true; return 0;
            case IDNO:
            case IDOK:
            case IDCANCEL: st->yes = false; st->done = true; return 0;
            }
        }
        break;
    case WM_CLOSE:
        if (st) { st->yes = false; st->done = true; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Modal Yes/No question that follows the host's dark / light mode. "No" is the default.
static bool themed_confirm(const std::wstring& text, const wchar_t* title) {
    ensure_class_and_font();
    static bool reg = false;
    static const wchar_t* cls = L"foo_liquidsoap_record_confirm";
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = confirm_proc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
        reg = true;
    }

    HWND owner = core_api::get_main_window();
    ConfirmState st;
    COLORREF bg = GetSysColor(COLOR_BTNFACE);
    if (!sample_host_background(owner, bg)) bg = GetSysColor(COLOR_BTNFACE);
    st.bg = bg;
    st.dark = luminance(bg) < 128;
    st.fg = st.dark ? RGB(240, 240, 240) : (COLORREF)GetSysColor(COLOR_BTNTEXT);
    st.brush = CreateSolidBrush(bg);

    // measure the text
    const int M = 16, TW = 440, BW = 84, BH = 26;
    RECT tr = {0, 0, TW, 0};
    {
        HDC dc = GetDC(nullptr);
        HGDIOBJ old = SelectObject(dc, g_font);
        DrawTextW(dc, text.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    int th = tr.bottom - tr.top;
    int cw = TW + 2 * M, ch = M + th + M + BH + M;

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT wr = {0, 0, cw, ch};
    AdjustWindowRectEx(&wr, style, FALSE, ex);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT orc{};
    if (!owner || !GetWindowRect(owner, &orc)) SetRect(&orc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    int x = orc.left + ((orc.right - orc.left) - ww) / 2, y = orc.top + ((orc.bottom - orc.top) - wh) / 2;

    HWND dlg = CreateWindowExW(ex, cls, title, style, x, y, ww, wh, owner, nullptr, core_api::get_my_instance(), &st);
    if (!dlg) { DeleteObject(st.brush); return false; }
    if (st.dark) {
        BOOL d = TRUE;
        DwmSetWindowAttribute(dlg, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof(d));
    }

    auto mk = [&](const wchar_t* c, const wchar_t* t, DWORD st2, int bx, int by, int bw, int bh, int id) {
        return CreateWindowExW(0, c, t, WS_CHILD | WS_VISIBLE | st2, bx, by, bw, bh, dlg, (HMENU)(INT_PTR)id,
                               core_api::get_my_instance(), nullptr);
    };
    mk(L"STATIC", text.c_str(), SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, M, M, TW, th, -1); // EDITCONTROL: long URLs wrap mid-word
    int by = M + th + M, bx = cw - M - BW;
    HWND bno = mk(L"BUTTON", L"No", BS_DEFPUSHBUTTON | WS_TABSTOP, bx, by, BW, BH, IDNO);
    HWND byes = mk(L"BUTTON", L"Yes", BS_PUSHBUTTON | WS_TABSTOP, bx - 8 - BW, by, BW, BH, IDYES);
    EnumChildWindows(dlg, set_font_proc, (LPARAM)g_font);
    if (st.dark) {
        SetWindowTheme(bno, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(byes, L"DarkMode_Explorer", nullptr);
    }

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(bno);
    MessageBeep(MB_ICONQUESTION);

    MSG m;
    while (!st.done) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r == 0) { PostQuitMessage((int)m.wParam); break; } // re-post quit for the host
        if (r < 0) break;
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    if (owner) SetForegroundWindow(owner);
    DeleteObject(st.brush);
    return st.yes;
}

// ============================================================
// DEFAULT UI ELEMENT
// ============================================================

class record_dui : public ui_element_instance {
public:
    record_dui(ui_element_config::ptr, ui_element_instance_callback::ptr cb) : m_callback(cb) {}
    ~record_dui() {
        if (m_wnd && IsWindow(m_wnd)) DestroyWindow(m_wnd);
    }

    void initialize_window(HWND parent) { m_wnd = view_create_child(parent); }
    HWND get_wnd() override { return m_wnd; }
    GUID get_guid() { return guid_dui; }
    GUID get_subclass() { return ui_element_subclass_utility; }
    void notify(const GUID&, t_size, const void*, t_size) {}
    void set_configuration(ui_element_config::ptr) override {}
    ui_element_config::ptr get_configuration() override {
        return ui_element_config::g_create_empty(guid_dui);
    }

private:
    HWND m_wnd = nullptr;
    ui_element_instance_callback::ptr m_callback;
};

class record_ui_element : public ui_element {
public:
    GUID get_guid() override { return guid_dui; }
    GUID get_subclass() override { return ui_element_subclass_utility; }
    void get_name(pfc::string_base& out) override { out = "Liquidsoap Record"; }
    ui_element_instance_ptr instantiate(HWND parent, ui_element_config::ptr cfg,
                                        ui_element_instance_callback::ptr cb) override {
        service_nnptr_t<service_impl_t<record_dui>> item = new service_impl_t<record_dui>(cfg, cb);
        item->initialize_window(parent);
        return item;
    }
    ui_element_config::ptr get_default_configuration() override {
        return ui_element_config::g_create_empty(guid_dui);
    }
    ui_element_children_enumerator_ptr enumerate_children(ui_element_config::ptr) override { return nullptr; }
    bool get_description(pfc::string_base& out) override {
        out = "Record internet radio streams with Liquidsoap.";
        return true;
    }
};
static service_factory_single_t<record_ui_element> g_dui_factory;

// ============================================================
// COLUMNS UI PANEL (only if the Columns UI SDK headers are found)
// ============================================================

#if FOO_RECORD_HAVE_CUI
class record_cui : public uie::window {
public:
    const GUID& get_extension_guid() const override { return guid_cui; }
    void get_name(pfc::string_base& out) const override { out = "Liquidsoap Record"; }
    void get_category(pfc::string_base& out) const override { out = "Panels"; }
    unsigned get_type() const override { return uie::type_panel; }
    bool is_available(const uie::window_host_ptr&) const override { return true; }

    HWND create_or_transfer_window(HWND parent, const uie::window_host_ptr& host,
                                   const ui_helpers::window_position_t& pos) override {
        if (m_wnd) {
            ShowWindow(m_wnd, SW_HIDE);
            SetParent(m_wnd, parent);
            m_host->relinquish_ownership(m_wnd);
            m_host = host;
            SetWindowPos(m_wnd, nullptr, pos.x, pos.y, pos.cx, pos.cy, SWP_NOZORDER);
        } else {
            m_host = host;
            m_wnd = view_create_child(parent);
            if (m_wnd) SetWindowPos(m_wnd, nullptr, pos.x, pos.y, pos.cx, pos.cy, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
        return m_wnd;
    }
    void destroy_window() override {
        if (m_wnd) DestroyWindow(m_wnd);
        m_wnd = nullptr;
        m_host.release();
    }
    HWND get_wnd() const override { return m_wnd; }

private:
    HWND m_wnd = nullptr;
    uie::window_host_ptr m_host;
};
static uie::window_factory<record_cui> g_cui_factory;
#endif

// ============================================================
// MENU / CONTEXT MENU / QUIT
// ============================================================

class record_mainmenu : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 1; }
    GUID get_command(t_uint32) override { return guid_menu; }
    void get_name(t_uint32, pfc::string_base& out) override { out = "Liquidsoap Record"; }
    bool get_description(t_uint32, pfc::string_base& out) override {
        out = "Open the Liquidsoap stream recorder.";
        return true;
    }
    GUID get_parent() override { return mainmenu_groups::view; }
    void execute(t_uint32, service_ptr_t<service_base>) override { ensure_popup(); }
};
static mainmenu_commands_factory_t<record_mainmenu> g_mainmenu_factory;

static bool is_stream_path(const char* p) {
    return p && (!_strnicmp(p, "http://", 7) || !_strnicmp(p, "https://", 8));
}

// ---- resolving the stream URL of a track ------------------------------------------
// Direct http(s) paths are used as-is. For .utag / .tags stub files (JSON, stream URL in "@")
// the file is read and the URL is taken from it.

static bool ends_with_i(const std::string& s, const char* ext) {
    size_t n = strlen(ext);
    return s.size() >= n && !_stricmp(s.c_str() + s.size() - n, ext);
}

// decodes the JSON string starting at t[i] == '"'; leaves i just past the closing quote
static bool json_string_at(const std::string& t, size_t& i, std::string& out) {
    out.clear();
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    while (i < t.size()) {
        char c = t[i++];
        if (c == '"') return true;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= t.size()) return false;
        char e = t[i++];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            if (i + 4 > t.size()) return false;
            unsigned cp = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                unsigned lo = (unsigned)strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
            }
            if (cp < 0x80) out.push_back((char)cp);
            else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
            else if (cp < 0x10000) {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
                out.push_back((char)(0xF0 | (cp >> 18)));
                out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default: out.push_back(e); break; // \" \\ \/
        }
    }
    return false;
}

struct StreamInfo {
    std::string url;  // from "@"
    std::string name; // from "name"
};

// Only the "@" key is used for the stream URL ("url"/"URL" are ignored); "name" gives the station name.
static StreamInfo json_find_stream_info(const std::string& t) {
    StreamInfo r;
    size_t i = 0;
    while (i < t.size()) {
        if (t[i] != '"') { ++i; continue; }
        std::string str;
        if (!json_string_at(t, i, str)) break;
        size_t j = i;
        while (j < t.size() && isspace((unsigned char)t[j])) ++j;
        if (j >= t.size() || t[j] != ':') continue; // a value, not a key
        ++j;
        while (j < t.size() && isspace((unsigned char)t[j])) ++j;
        if (j < t.size() && t[j] == '"') {
            std::string val;
            size_t k = j;
            if (!json_string_at(t, k, val)) break;
            if (str == "@" && r.url.empty()) r.url = val;
            else if (str == "name" && r.name.empty()) r.name = val;
            i = k;
        } else {
            i = j;
        }
    }
    if (r.url.find("://") == std::string::npos) r.url.clear();
    return r;
}

static StreamInfo resolve_stream_info(const char* path) {
    StreamInfo none;
    if (!path || !*path) return none;
    if (is_stream_path(path)) { StreamInfo d; d.url = path; return d; }

    std::string p = path;
    if (!_strnicmp(p.c_str(), "file://", 7)) p.erase(0, 7);
    if (!ends_with_i(p, ".utag") && !ends_with_i(p, ".tags")) return none;

    HANDLE h = CreateFileW(W(p.c_str()).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return none;
    std::string data;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 1024 * 1024) {
        data.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, &data[0], (DWORD)data.size(), &got, nullptr)) got = 0;
        data.resize(got);
    }
    CloseHandle(h);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    return json_find_stream_info(data);
}

class record_context : public contextmenu_item_simple {
public:
    unsigned get_num_items() override { return 2; }
    void get_item_name(unsigned i, pfc::string_base& out) override {
        out = (i == 0) ? "Record with Liquidsoap..." : "Record with Liquidsoap (start now)";
    }
    void get_item_default_path(unsigned, pfc::string_base& out) override { out = ""; }
    GUID get_item_guid(unsigned i) override { return i == 0 ? guid_ctx_open : guid_ctx_start; }
    bool get_item_description(unsigned i, pfc::string_base& out) override {
        out = (i == 0) ? "Open the Liquidsoap recorder with this stream URL."
                       : "Start recording this stream with the saved Liquidsoap settings.";
        return true;
    }
    bool context_get_display(unsigned i, metadb_handle_list_cref data, pfc::string_base& out, unsigned&,
                             const GUID&) override {
        if (data.get_count() != 1) return false;
        if (resolve_stream_info(data[0]->get_path()).url.empty()) return false;
        get_item_name(i, out);
        return true;
    }
    void context_command(unsigned i, metadb_handle_list_cref data, const GUID&) override {
        if (data.get_count() != 1) return;
        const char* path = data[0]->get_path();
        StreamInfo si = resolve_stream_info(path);
        if (si.url.empty()) return;

        // Already recording something else: ask before replacing it.
        if (g_running && strcmp(c_url.get_ptr(), si.url.c_str()) != 0) {
            std::wstring msg = L"Already recording:\n" + W(c_url.get_ptr()) +
                               L"\n\nStop the current recording and switch to:\n" + W(si.url.c_str()) + L"?";
            if (!themed_confirm(msg, L"Liquidsoap Record")) return; // keep the current recording and settings untouched
            if (g_running) {
                if (!g_stopping.exchange(true)) stop_worker();
                while (g_stopping) Sleep(50);
            }
        }

        c_url.set_string(si.url.c_str());
        if (!is_stream_path(path)) c_station.set_string(si.name.c_str()); // stub: name from "name" (empty if none)
        reload_all(nullptr);
        show_ui();
        if (i == 1) start_recording();
    }
};
static contextmenu_item_factory_t<record_context> g_context_factory;

class record_initquit : public initquit {
public:
    void on_init() override {}
    void on_quit() override {
        if (g_popup && IsWindow(g_popup)) {
            if (View* v = V(g_popup)) save_from_controls(v);
            save_popup_placement(g_popup);
        }
        shutdown_sync();
        if (g_popup && IsWindow(g_popup)) DestroyWindow(g_popup);
    }
};
static initquit_factory_t<record_initquit> g_initquit_factory;

} // namespace
