// Windows tray icon: LunaShare keeps running after its window is closed (when that setting is on). Clicking the
// icon brings the window back; its menu has "Open" and "Quit". New messages / calls show as balloons.
#include "tray.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>

#include "i18n.h"

namespace tray {

static HWND g_wnd = nullptr;
static NOTIFYICONDATAW g_nid{};
static bool g_added = false;
static std::atomic<bool> g_show{false}, g_quit{false};
static std::mutex g_m;
static const UINT kMsg = WM_APP + 21;

static std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == kMsg) {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK || ev == NIN_BALLOONUSERCLICK) g_show = true;
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, W(T("Open LunaShare")).c_str());
            AppendMenuW(menu, MF_STRING, 2, W(T("Quit")).c_str());
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) g_show = true;
            if (cmd == 2) g_quit = true;
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool Available() { return true; }

bool Show(const std::string& tooltip) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_added) return true;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_wnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"LunaShareTray";
        RegisterClassW(&wc);
        g_wnd = CreateWindowW(L"LunaShareTray", L"LunaShare", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        if (!g_wnd) return false;
    }
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kMsg;
    g_nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(2), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, W(tooltip).c_str(), _TRUNCATE);
    g_added = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    return g_added;
}

void Hide() {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_added) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_added = false;
}

void Balloon(const std::string& title, const std::string& text) {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_added) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, W(title).c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, W(text.empty() ? std::string(" ") : text).c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

bool TakeShow() { return g_show.exchange(false); }
bool TakeQuit() { return g_quit.exchange(false); }

}  // namespace tray

#else

namespace tray {
bool Available() { return false; }
bool Show(const std::string&) { return false; }
void Hide() {}
void Balloon(const std::string&, const std::string&) {}
bool TakeShow() { return false; }
bool TakeQuit() { return false; }
}  // namespace tray

#endif
