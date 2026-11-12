#include "core.h"
#include "resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cstdio>

namespace tc {
constexpr UINT TrayMessage = WM_APP + 1;
static HINSTANCE instance() { return GetModuleHandleW(nullptr); }
static std::wstring text(HWND window) { int n = GetWindowTextLengthW(window); std::wstring s(n + 1, 0); GetWindowTextW(window, s.data(), n + 1); s.resize(n); return s; }
static void put(HWND window, int id, const std::wstring& s) { SetDlgItemTextW(window, id, s.c_str()); }
static std::wstring number(double n) { auto s = std::to_wstring(n); while (s.back() == '0') s.pop_back(); if (s.back() == '.') s.pop_back(); return s; }
static double numeric(HWND w, int id, double low, double high, bool integer = true) {
    auto s = text(GetDlgItem(w, id)); size_t count = 0; double n;
    try { n = std::stod(s, &count); } catch (...) { throw std::runtime_error("Введите корректное число в настройках"); }
    if (count != s.size() || !std::isfinite(n) || n < low || n > high || (integer && n != std::floor(n))) throw std::runtime_error("Число вне допустимого диапазона настроек"); return n;
}
static std::vector<HMONITOR> monitors() {
    std::vector<HMONITOR> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL { ((std::vector<HMONITOR>*)p)->push_back(m); return TRUE; }, (LPARAM)&result);
    if (result.empty()) result.push_back(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)); return result;
}
static void centerDialog(HWND window) {
    POINT cursor; GetCursorPos(&cursor); MONITORINFO info{sizeof(info)};
    check(GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info) != 0, "Dialog monitor");
    RECT rect; GetWindowRect(window, &rect); auto area = info.rcWork;
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    SetWindowPos(window, nullptr, area.left + std::max(0L, (area.right - area.left - width) / 2), area.top + std::max(0L, (area.bottom - area.top - height) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static LOGFONTW fontDescription(const Settings& s, UINT dpi) {
    LOGFONTW f{}; f.lfHeight = -(int)std::lround(s.fontSize * dpi / 72.0); f.lfWeight = FW_NORMAL;
    f.lfCharSet = DEFAULT_CHARSET; f.lfQuality = ANTIALIASED_QUALITY; wcscpy_s(f.lfFaceName, s.fontName.c_str()); return f;
}
struct App {
    Settings settings, draft;
    Ledger ledger;
    HWND main = nullptr, overlay = nullptr, settingsWindow = nullptr, historyWindow = nullptr;
    HFONT overlayFont = nullptr;
    HICON icon = nullptr;
    NOTIFYICONDATAW tray{};
    std::unique_ptr<ApiServer> api;
    bool testing = false, registered = false, closing = false, sleeping = false, available = false, menuOpen = false, trayAdded = false;
    Time previous = now(), previousInput = 0;
    unsigned ticks = 0;
    int historyDays = 30;
    BYTE overlayAlpha = 255;
    std::wstring overlayText;
    std::string error;
    std::atomic<HMENU> activeMenu{nullptr};
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    App(Settings initial, const std::wstring& file, bool test = false) : settings(std::move(initial)), ledger(file), testing(test) {
        try {
            settings.validate(); previousInput = lastInput(previous); available = sessionAvailable();
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS}; check(InitCommonControlsEx(&controls) != 0, "Common controls");
            icon = (HICON)LoadImageW(instance(), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_SHARED);
            check(icon != nullptr, "Application icon");
            WNDCLASSEXW klass{sizeof(klass)}; klass.hInstance = instance(); klass.lpfnWndProc = ownerProc; klass.lpszClassName = L"TimeCounter.Native.Owner"; klass.hIcon = icon;
            check(RegisterClassExW(&klass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register owner window");
            klass.lpfnWndProc = overlayProc; klass.lpszClassName = L"TimeCounter.Native.Overlay"; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            check(RegisterClassExW(&klass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register overlay window");
            // TrackPopupMenu requires a foreground-capable owner. A truly hidden
            // window cannot reliably acquire foreground status on the first open.
            // Keep this tool window invisible to the eye, but eligible for activation.
            main = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"TimeCounter.Native.Owner", L"TimeCounter", WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, instance(), this); check(main != nullptr, "Create owner window");
            check(SetLayeredWindowAttributes(main, 0, 0, LWA_ALPHA) != 0, "Tray owner opacity");
            ShowWindow(main, SW_SHOWNOACTIVATE);
            // StartupInfo may override the first ShowWindow (e.g. a hidden launch).
            check(SetWindowPos(main, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != 0, "Tray owner visibility");
            overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED, L"TimeCounter.Native.Overlay", L"TimeCounter overlay", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance(), this); check(overlay != nullptr, "Create overlay");
            check(WTSRegisterSessionNotification(main, NOTIFY_FOR_THIS_SESSION) != 0, "WTS session notifications"); registered = true;
            tray.cbSize = sizeof(tray); tray.hWnd = main; tray.uID = 1; tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = TrayMessage; tray.hIcon = icon;
            wcscpy_s(tray.szTip, L"TimeCounter"); addTray();
            if (!testing) settings.save();
            try { api = makeApi(settings); } catch (const std::exception& e) { notify(L"HTTP API не запущен", wide(e.what())); }
            updateOverlay(); check(SetTimer(main, 1, 1000, nullptr) != 0, "Accounting timer");
        } catch (...) { cleanup(); throw; }
    }
    ~App() { cleanup(); }
    void cleanup() noexcept {
        api.reset();
        if (trayAdded) { Shell_NotifyIconW(NIM_DELETE, &tray); trayAdded = false; }
        if (main) { KillTimer(main, 1); if (registered) WTSUnRegisterSessionNotification(main); registered = false; }
        if (settingsWindow) DestroyWindow(settingsWindow);
        if (historyWindow) DestroyWindow(historyWindow);
        if (overlay) { DestroyWindow(overlay); overlay = nullptr; }
        if (main) { DestroyWindow(main); main = nullptr; }
        if (overlayFont) { DeleteObject(overlayFont); overlayFont = nullptr; }
    }
    void addTray() { trayAdded = Shell_NotifyIconW(NIM_ADD, &tray) != 0; }
    void notify(const std::wstring& title, const std::wstring& message) {
        if (testing) return;
        logError(utf8(title + L": " + message));
        tray.uFlags = NIF_INFO; wcsncpy_s(tray.szInfoTitle, title.c_str(), _TRUNCATE); wcsncpy_s(tray.szInfo, message.c_str(), _TRUNCATE); tray.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &tray); tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    }
    std::unique_ptr<ApiServer> makeApi(const Settings& s) { return std::make_unique<ApiServer>(s.port, s.apiToken, [this](int days) { return historyJson(ledger, days, now()); }); }
    void saveSettings() { if (!testing) settings.save(); }
    void tick() {
        Time time = now(), input = lastInput(time), timeout = Time(settings.idleMinutes) * 60 * Second;
        if (time - previous > 10 * Second) ledger.add(previous, time, false);
        else if (time > previous) {
            if (available && !sleeping && input > previous && input <= time) {
                record(ledger, previous, input, true, previousInput, timeout); record(ledger, input, time, true, input, timeout);
            } else record(ledger, previous, time, available && !sleeping, input > previous ? previousInput : input, timeout);
        }
        previous = time; previousInput = input; available = !sleeping && sessionAvailable();
        updateOverlay();
    }
    void updateOverlay() {
        auto row = ledger.history(0, now(), localZone()).front(); auto value = duration(row.active);
        if (!menuOpen && overlay) {
            auto screens = monitors(); HMONITOR screen = screens[std::min<size_t>(settings.monitor, screens.size() - 1)];
            MONITORINFO info{sizeof(info)}; check(GetMonitorInfoW(screen, &info) != 0, "Monitor information");
            UINT dpiX = 96, dpiY = 96; GetDpiForMonitor(screen, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
            auto description = fontDescription(settings, dpiY); HFONT font = CreateFontIndirectW(&description); check(font != nullptr, "Overlay font");
            HDC dc = GetDC(overlay); auto old = SelectObject(dc, font); SIZE size{}; GetTextExtentPoint32W(dc, value.c_str(), (int)value.size(), &size); SelectObject(dc, old); ReleaseDC(overlay, dc);
            if (overlayFont) DeleteObject(overlayFont); overlayFont = font; overlayText = value;
            auto area = info.rcWork; int w = std::min<int>(area.right - area.left, size.cx + 12), h = std::min<int>(area.bottom - area.top, size.cy + 8);
            bool right = settings.corner.find(L"Справа") == 0, bottom = settings.corner.find(L"снизу") != std::wstring::npos;
            int x = right ? area.right - w - settings.offsetX : area.left + settings.offsetX;
            int y = bottom ? area.bottom - h - settings.offsetY : area.top + settings.offsetY;
            x = std::clamp(x, (int)area.left, (int)area.right - w); y = std::clamp(y, (int)area.top, (int)area.bottom - h);
            RECT current{}; GetWindowRect(overlay, &current);
            if (current.left != x || current.top != y || current.right - current.left != w || current.bottom - current.top != h)
                SetWindowPos(overlay, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
            BYTE alpha = BYTE(std::lround((100 - settings.transparency) * 255.0 / 100));
            if (overlayAlpha != alpha || !ticks) { check(SetLayeredWindowAttributes(overlay, RGB(1, 2, 3), alpha, LWA_COLORKEY | LWA_ALPHA) != 0, "Overlay opacity"); overlayAlpha = alpha; }
            if (settings.overlayVisible != (IsWindowVisible(overlay) != 0)) ShowWindow(overlay, settings.overlayVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
            InvalidateRect(overlay, nullptr, FALSE);
        }
        std::wstring tip = L"TimeCounter " + value + (available && previous - previousInput < Time(settings.idleMinutes) * 60 * Second ? L" — активно" : L" — неактивно");
        wcsncpy_s(tray.szTip, tip.c_str(), _TRUNCATE); tray.uFlags = NIF_TIP; Shell_NotifyIconW(NIM_MODIFY, &tray); tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    }
    void showMenu() {
        if (menuOpen || closing) return; activeMenu = CreatePopupMenu(); check(activeMenu != nullptr, "Create tray menu"); menuOpen = true;
        AppendMenuW(activeMenu, MF_STRING, 1, settings.overlayVisible ? L"Скрыть цифры" : L"Показать цифры");
        AppendMenuW(activeMenu, MF_STRING, 2, L"Настройки"); AppendMenuW(activeMenu, MF_STRING, 3, L"История"); AppendMenuW(activeMenu, MF_SEPARATOR, 0, nullptr); AppendMenuW(activeMenu, MF_STRING, 4, L"Выйти");
        POINT p; GetCursorPos(&p); SetForegroundWindow(main);
        UINT command = TrackPopupMenuEx(activeMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, main, nullptr);
        PostMessageW(main, WM_NULL, 0, 0); DestroyMenu(activeMenu); activeMenu = nullptr; menuOpen = false;
        switch (command) {
        case 1: settings.overlayVisible = !settings.overlayVisible; saveSettings(); updateOverlay(); break;
        case 2: openSettings(); break; case 3: openHistory(); break; case 4: close(); break;
        }
    }
    void openSettings() {
        if (settingsWindow) { ShowWindow(settingsWindow, SW_RESTORE); SetForegroundWindow(settingsWindow); return; }
        tick(); draft = settings; settingsWindow = CreateDialogParamW(instance(), MAKEINTRESOURCEW(IDD_SETTINGS), main, settingsProc, (LPARAM)this);
        check(settingsWindow != nullptr, "Create settings dialog"); centerDialog(settingsWindow); ShowWindow(settingsWindow, SW_SHOW); SetForegroundWindow(settingsWindow);
    }
    void openHistory() {
        if (historyWindow) { ShowWindow(historyWindow, SW_RESTORE); SetForegroundWindow(historyWindow); return; }
        historyWindow = CreateDialogParamW(instance(), MAKEINTRESOURCEW(IDD_HISTORY), main, historyProc, (LPARAM)this);
        check(historyWindow != nullptr, "Create history dialog"); centerDialog(historyWindow); ShowWindow(historyWindow, SW_SHOW); SetForegroundWindow(historyWindow);
    }
    void close() {
        if (closing) return; closing = true;
        try { tick(); ledger.save(); saveSettings(); } catch (const std::exception& e) { error = e.what(); if (!testing) MessageBoxW(main, wide(error).c_str(), L"TimeCounter: ошибка сохранения", MB_ICONERROR); }
        cleanup(); if (!testing) PostQuitMessage(0);
    }
    void paintOverlay(HWND w) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); RECT r{}; GetClientRect(w, &r);
        HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, r.right), std::max(1L, r.bottom));
        auto oldBitmap = SelectObject(memory, bitmap); HBRUSH brush = CreateSolidBrush(RGB(1, 2, 3)); FillRect(memory, &r, brush); DeleteObject(brush);
        auto oldFont = SelectObject(memory, overlayFont); SetBkMode(memory, TRANSPARENT); SetTextColor(memory, settings.color());
        DrawTextW(memory, overlayText.c_str(), (int)overlayText.size(), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY); SelectObject(memory, oldFont); SelectObject(memory, oldBitmap); DeleteObject(bitmap); DeleteDC(memory); EndPaint(w, &ps);
    }
    void initSettings(HWND w) {
        SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_APPWINDOW);
        SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)icon); SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        put(w, IDC_IDLE, std::to_wstring(draft.idleMinutes)); put(w, IDC_FONT, draft.fontName); put(w, IDC_FONTSIZE, number(draft.fontSize));
        put(w, IDC_ALPHA, std::to_wstring(draft.transparency)); put(w, IDC_X, std::to_wstring(draft.offsetX)); put(w, IDC_Y, std::to_wstring(draft.offsetY));
        put(w, IDC_MONITOR, std::to_wstring(std::min<int>(draft.monitor, (int)monitors().size() - 1) + 1)); put(w, IDC_PORT, std::to_wstring(draft.port)); put(w, IDC_TOKEN, wide(draft.apiToken));
        const wchar_t* corners[] = {L"Справа сверху", L"Слева сверху", L"Справа снизу", L"Слева снизу"};
        for (int i = 0; i < 4; ++i) { SendDlgItemMessageW(w, IDC_CORNER, CB_ADDSTRING, 0, (LPARAM)corners[i]); if (draft.corner == corners[i]) SendDlgItemMessageW(w, IDC_CORNER, CB_SETCURSEL, i, 0); }
        CheckDlgButton(w, IDC_AUTO, !testing && autoStart() ? BST_CHECKED : BST_UNCHECKED); updateColor(w);
    }
    void updateColor(HWND w) { wchar_t b[40]; swprintf_s(b, L"Цвет: #%06X", draft.colorArgb & 0xffffff); put(w, IDC_COLOR, b); }
    void applySettings(HWND w) {
        Settings candidate = draft;
        candidate.idleMinutes = (int)numeric(w, IDC_IDLE, 1, 1440); candidate.fontSize = numeric(w, IDC_FONTSIZE, 8, 500, false); candidate.transparency = (int)numeric(w, IDC_ALPHA, 0, 99);
        candidate.monitor = (int)numeric(w, IDC_MONITOR, 1, (double)monitors().size()) - 1; candidate.offsetX = (int)numeric(w, IDC_X, 0, 10000); candidate.offsetY = (int)numeric(w, IDC_Y, 0, 10000); candidate.port = (int)numeric(w, IDC_PORT, 1024, 65535);
        candidate.corner = text(GetDlgItem(w, IDC_CORNER)); candidate.validate();
        std::unique_ptr<ApiServer> replacement;
        if (candidate.port != settings.port || !api) replacement = makeApi(candidate);
        tick();
        if (!testing) {
            bool before = autoStart();
            try { setAutoStart(IsDlgButtonChecked(w, IDC_AUTO) == BST_CHECKED); candidate.save(); }
            catch (...) { try { setAutoStart(before); settings.save(); } catch (...) { } throw; }
        }
        settings = candidate; if (replacement) api.swap(replacement); updateOverlay(); DestroyWindow(w);
    }
    void fillHistory(HWND w) {
        auto rows = ledger.history(historyDays, now(), localZone()); HWND list = GetDlgItem(w, IDC_LIST);
        int top = ListView_GetTopIndex(list), selected = ListView_GetNextItem(list, -1, LVNI_SELECTED); SendMessageW(list, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(list);
        int i = 0;
        for (const auto& row : rows) {
            auto date = wide(row.date), active = duration(row.active), inactive = duration(row.inactive), uncounted = duration(row.uncounted);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = date.data(); ListView_InsertItem(list, &item);
            ListView_SetItemText(list, i, 1, active.data()); ListView_SetItemText(list, i, 2, inactive.data()); ListView_SetItemText(list, i, 3, uncounted.data()); ++i;
        }
        if (selected >= 0 && selected < i) ListView_SetItemState(list, selected, LVIS_SELECTED, LVIS_SELECTED);
        if (top > 0 && top < i) { RECT r{}; if (ListView_GetItemRect(list, 0, &r, LVIR_BOUNDS)) ListView_Scroll(list, 0, top * (r.bottom - r.top)); }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
    }
    static LRESULT CALLBACK ownerProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto a = (App*)GetWindowLongPtrW(w, GWLP_USERDATA);
        if (m == WM_NCCREATE) { a = (App*)((CREATESTRUCTW*)lp)->lpCreateParams; SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)a); }
        if (!a) return DefWindowProcW(w, m, wp, lp);
        try {
            if (m == a->taskbarCreated) { a->addTray(); return 0; }
            switch (m) {
            case TrayMessage: if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) a->showMenu(); else if (lp == WM_LBUTTONDBLCLK) a->openHistory(); return 0;
            case WM_TIMER: a->tick(); if (++a->ticks % 15 == 0) a->ledger.save(); return 0;
            case WM_WTSSESSION_CHANGE:
                a->tick(); if (wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT || wp == WTS_SESSION_LOGOFF || wp == WTS_SESSION_LOCK) a->available = false;
                else if (wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_LOGON || wp == WTS_SESSION_UNLOCK) a->available = sessionAvailable(); return 0;
            case WM_POWERBROADCAST:
                if (wp == PBT_APMSUSPEND) { a->tick(); a->sleeping = true; a->available = false; }
                else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) { a->tick(); a->sleeping = false; a->available = sessionAvailable(); } return TRUE;
            case WM_QUERYENDSESSION: a->tick(); a->ledger.save(); return TRUE;
            case WM_ENDSESSION: if (wp) a->close(); return 0;
            case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: a->updateOverlay(); return 0;
            case WM_CLOSE: a->close(); return 0;
            }
        } catch (const std::exception& e) {
            a->error = e.what(); logError(a->error);
            if (m == WM_TIMER) KillTimer(w, 1);
            a->notify(m == WM_TIMER ? L"Учёт остановлен" : L"TimeCounter: ошибка", wide(a->error));
            if (m == WM_QUERYENDSESSION) return TRUE;
        }
        return DefWindowProcW(w, m, wp, lp);
    }
    static LRESULT CALLBACK overlayProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto a = (App*)GetWindowLongPtrW(w, GWLP_USERDATA);
        if (m == WM_NCCREATE) { a = (App*)((CREATESTRUCTW*)lp)->lpCreateParams; SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)a); }
        if (m == WM_NCHITTEST) return HTTRANSPARENT; if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_PAINT && a) { a->paintOverlay(w); return 0; }
        return DefWindowProcW(w, m, wp, lp);
    }
    static INT_PTR CALLBACK settingsProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto a = (App*)GetWindowLongPtrW(w, DWLP_USER);
        if (m == WM_INITDIALOG) { a = (App*)lp; SetWindowLongPtrW(w, DWLP_USER, (LONG_PTR)a); }
        if (!a) return FALSE;
        try {
            if (m == WM_INITDIALOG) { a->initSettings(w); return TRUE; }
            if (m == WM_COMMAND) {
                switch (LOWORD(wp)) {
                case IDC_PICKFONT: {
                    LOGFONTW f = fontDescription(a->draft, GetDpiForWindow(w)); CHOOSEFONTW choose{sizeof(choose)}; choose.hwndOwner = w; choose.lpLogFont = &f; choose.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST | CF_LIMITSIZE; choose.nSizeMin = 8; choose.nSizeMax = 500;
                    if (ChooseFontW(&choose)) { a->draft.fontName = f.lfFaceName; a->draft.fontSize = choose.iPointSize / 10.0; put(w, IDC_FONT, a->draft.fontName); put(w, IDC_FONTSIZE, number(a->draft.fontSize)); } return TRUE;
                }
                case IDC_COLOR: {
                    static COLORREF custom[16]{}; CHOOSECOLORW choose{sizeof(choose)}; choose.hwndOwner = w; choose.rgbResult = a->draft.color(); choose.lpCustColors = custom; choose.Flags = CC_FULLOPEN | CC_RGBINIT;
                    if (ChooseColorW(&choose)) { a->draft.colorArgb = 0xff000000 | (GetRValue(choose.rgbResult) << 16) | (GetGValue(choose.rgbResult) << 8) | GetBValue(choose.rgbResult); a->updateColor(w); } return TRUE;
                }
                case IDOK: a->applySettings(w); return TRUE;
                case IDCANCEL: DestroyWindow(w); return TRUE;
                }
            }
            if (m == WM_CLOSE) { DestroyWindow(w); return TRUE; }
            if (m == WM_NCDESTROY) a->settingsWindow = nullptr;
        } catch (const std::exception& e) { a->error = e.what(); if (!a->testing) MessageBoxW(w, wide(a->error).c_str(), L"Настройки не сохранены", MB_ICONERROR); }
        return FALSE;
    }
    static INT_PTR CALLBACK historyProc(HWND w, UINT m, WPARAM wp, LPARAM lp) noexcept {
        auto a = (App*)GetWindowLongPtrW(w, DWLP_USER);
        if (m == WM_INITDIALOG) { a = (App*)lp; SetWindowLongPtrW(w, DWLP_USER, (LONG_PTR)a); }
        if (!a) return FALSE;
        try {
            if (m == WM_INITDIALOG) {
                SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_APPWINDOW);
                SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)a->icon); SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)a->icon);
                HWND list = GetDlgItem(w, IDC_LIST); ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
                const wchar_t* names[] = {L"Дата", L"Активно", L"Неактивно", L"Непосчитано"};
                for (int i = 0; i < 4; ++i) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)names[i]; c.cx = MulDiv(140, GetDpiForWindow(w), 96); ListView_InsertColumn(list, i, &c); }
                SendDlgItemMessageW(w, IDC_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(w, IDC_DAYS), 0); SendDlgItemMessageW(w, IDC_SPIN, UDM_SETRANGE32, 0, 3660);
                put(w, IDC_DAYS, std::to_wstring(a->historyDays)); a->fillHistory(w); SetTimer(w, 1, 5000, nullptr); return TRUE;
            }
            if (m == WM_TIMER) { a->fillHistory(w); return TRUE; }
            if (m == WM_COMMAND && LOWORD(wp) == IDC_DAYS && HIWORD(wp) == EN_CHANGE) {
                auto s = text(GetDlgItem(w, IDC_DAYS));
                if (!s.empty() && s.size() <= 4 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= '0' && c <= '9'; })) {
                    int days = std::stoi(s); if (days <= 3660) { a->historyDays = days; a->fillHistory(w); }
                } return TRUE;
            }
            if (m == WM_SIZE) {
                HWND list = GetDlgItem(w, IDC_LIST); if (list) { RECT r; GetClientRect(w, &r); POINT p{0, 0}; MapWindowPoints(list, w, &p, 1); int margin = MulDiv(8, GetDpiForWindow(w), 96); int width = std::max(100L, r.right - margin * 2); MoveWindow(list, margin, p.y, width, std::max(40L, r.bottom - p.y - margin), TRUE); for (int i = 0; i < 4; ++i) ListView_SetColumnWidth(list, i, std::max(80, width / 4 - 2)); } return TRUE;
            }
            if (m == WM_GETMINMAXINFO) { auto p = (MINMAXINFO*)lp; p->ptMinTrackSize = POINT{MulDiv(480, GetDpiForWindow(w), 96), MulDiv(260, GetDpiForWindow(w), 96)}; return TRUE; }
            if (m == WM_CLOSE || (m == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK))) { DestroyWindow(w); return TRUE; }
            if (m == WM_NCDESTROY) { KillTimer(w, 1); a->historyWindow = nullptr; }
        } catch (const std::exception& e) { a->error = e.what(); a->notify(L"Ошибка истории", wide(a->error)); }
        return FALSE;
    }
    int run() {
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (settingsWindow && IsDialogMessageW(settingsWindow, &message)) continue;
            if (historyWindow && IsDialogMessageW(historyWindow, &message)) continue;
            TranslateMessage(&message); DispatchMessageW(&message);
        } return (int)message.wParam;
    }
};
#ifndef TIMECOUNTER_TEST
static std::wstring mutexName() {
    HANDLE token; check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0, "User token"); DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes); std::vector<BYTE> buffer(bytes);
    bool ok = GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) != 0; CloseHandle(token); check(ok, "User SID");
    LPWSTR sid = nullptr; check(ConvertSidToStringSidW(((TOKEN_USER*)buffer.data())->User.Sid, &sid) != 0, "User SID string");
    std::wstring name = L"Global\\TimeCounter-"; name += sid; LocalFree(sid); return name;
}
#endif
#ifdef TIMECOUNTER_TEST
static void uiExpect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static void pump(DWORD milliseconds) {
    ULONGLONG until = GetTickCount64() + milliseconds;
    do { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(10); } while (GetTickCount64() < until);
}
void runUiTests(const std::wstring& folder, int port) {
    Settings settings; settings.port = port; settings.apiToken = "isolated-ui-test-token-0123456789";
    App app(settings, folder + L"\\ui-history.json", true);
    uiExpect(app.api != nullptr, "UI API startup");
    POINT cursor; GetCursorPos(&cursor);
    struct RestoreCursor { POINT point; ~RestoreCursor() { SetCursorPos(point.x, point.y); } } restore{cursor};
    // A real Explorer tray click grants foreground permission. Our direct callback
    // has no Explorer in the call chain, so first deliver input to a test window
    // in this process to reproduce the same foreground permission legitimately.
    HWND inputOwner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"TimeCounter — проверка меню", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 120, nullptr, nullptr, instance(), nullptr);
    uiExpect(inputOwner != nullptr, "Test input owner");
    struct DestroyInputOwner { HWND window; ~DestroyInputOwner() { DestroyWindow(window); } } destroyInputOwner{inputOwner};
    pump(100);
    LONG_PTR style = GetWindowLongPtrW(app.overlay, GWL_EXSTYLE);
    uiExpect((style & (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST)) == (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST), "Overlay window styles");
    uiExpect(SendMessageW(app.overlay, WM_NCHITTEST, 0, 0) == HTTRANSPARENT, "Overlay click transparency");
    uiExpect(SendMessageW(app.overlay, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "Overlay cannot activate");
    COLORREF key; BYTE alpha; DWORD flags;
    uiExpect(GetLayeredWindowAttributes(app.overlay, &key, &alpha, &flags) && alpha == 64 && key == RGB(1, 2, 3), "Overlay opacity");
    uiExpect(app.overlayText.find(L':') == app.overlayText.rfind(L':'), "Overlay has no seconds");
    auto click = [&](UINT index) {
        SetWindowPos(inputOwner, HWND_TOPMOST, 100, 100, 320, 120, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        SetCursorPos(140, 110);
        INPUT activate[2]{}; activate[0].type = activate[1].type = INPUT_MOUSE; activate[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; activate[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        uiExpect(SendInput(2, activate, sizeof(INPUT)) == 2, "Activate test input owner"); pump(100);
        SetWindowPos(inputOwner, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetCursorPos(200, 200); HWND owner = app.main;
        std::exception_ptr error;
        std::thread mouse([&] {
            try {
                Sleep(1200); RECT bounds; HMENU menu = app.activeMenu.load();
                if (!menu || !GetMenuItemRect(nullptr, menu, index, &bounds)) throw std::runtime_error("Tray menu closed before click, item " + std::to_string(index));
                SetCursorPos(bounds.left + 20, (bounds.top + bounds.bottom) / 2); Sleep(50);
                INPUT input[2]{}; input[0].type = input[1].type = INPUT_MOUSE; input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                uiExpect(SendInput(2, input, sizeof(INPUT)) == 2, "Send tray click"); Sleep(300);
                if (app.activeMenu.load()) throw std::runtime_error("Tray did not accept mouse click");
            } catch (...) { error = std::current_exception(); PostMessageW(owner, WM_CANCELMODE, 0, 0); }
        });
        // Exercise the actual notification callback as well as native mouse selection.
        SendMessageW(owner, TrayMessage, 1, WM_RBUTTONUP); mouse.join(); if (error) std::rethrow_exception(error); pump(250);
        uiExpect(app.error.empty(), app.error.c_str());
    };
    for (int i = 0; i < 2; ++i) {
        click(0); pump(1200); uiExpect(!IsWindowVisible(app.overlay), "Tray hide persists over timer updates");
        click(0); pump(1200); uiExpect(IsWindowVisible(app.overlay) != 0, "Tray show persists over timer updates");
    }
    click(1); uiExpect(app.settingsWindow && IsWindowVisible(app.settingsWindow), "Tray settings");
    RECT dialogRect; GetWindowRect(app.settingsWindow, &dialogRect); RECT screenRect{};
    MONITORINFO dialogMonitor{sizeof(dialogMonitor)}; GetMonitorInfoW(MonitorFromWindow(app.settingsWindow, MONITOR_DEFAULTTONEAREST), &dialogMonitor); screenRect = dialogMonitor.rcWork;
    uiExpect(dialogRect.left >= screenRect.left && dialogRect.top >= screenRect.top && dialogRect.left < screenRect.right && dialogRect.top < screenRect.bottom, "Settings opens on screen, independent of tray owner");
    uiExpect((HICON)SendMessageW(app.settingsWindow, WM_GETICON, ICON_BIG, 0) == app.icon, "Settings application icon");
    put(app.settingsWindow, IDC_FONTSIZE, L"96"); put(app.settingsWindow, IDC_ALPHA, L"83"); put(app.settingsWindow, IDC_X, L"44");
    SendMessageW(app.settingsWindow, WM_COMMAND, IDOK, 0); pump(100);
    uiExpect(!app.settingsWindow && app.settings.fontSize == 96 && app.settings.transparency == 83 && app.settings.offsetX == 44, "Settings apply");
    uiExpect(GetLayeredWindowAttributes(app.overlay, &key, &alpha, &flags) && alpha == 43, "Changed overlay opacity");
    click(1); SendMessageW(app.settingsWindow, WM_COMMAND, IDCANCEL, 0); pump(50); uiExpect(!app.settingsWindow, "Settings cancel and reopen");
    click(2); uiExpect(app.historyWindow && IsWindowVisible(app.historyWindow), "Tray history");
    HWND list = GetDlgItem(app.historyWindow, IDC_LIST); uiExpect(ListView_GetItemCount(list) == 31, "History default range");
    put(app.historyWindow, IDC_DAYS, L"3"); pump(50); uiExpect(ListView_GetItemCount(list) == 4, "History selected range");
    wchar_t value[100]; ListView_GetItemText(list, 0, 1, value, 100); uiExpect(std::wstring(value).size() == 5, "History HH:mm format");
    SendMessageW(app.historyWindow, WM_CLOSE, 0, 0); pump(50); uiExpect(!app.historyWindow, "History close");
    SendMessageW(app.main, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0); uiExpect(!app.available, "WTS lock notification");
    SendMessageW(app.main, WM_POWERBROADCAST, PBT_APMSUSPEND, 0); uiExpect(app.sleeping && !app.available, "Suspend notification");
    SendMessageW(app.main, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0); uiExpect(!app.sleeping, "Resume notification");
    click(4); uiExpect(app.closing && !app.main && !app.overlay && !app.trayAdded, "Tray exit and cleanup");
    uiExpect(GetFileAttributesW((folder + L"\\ui-history.json").c_str()) != INVALID_FILE_ATTRIBUTES, "Exit saves history");
}
#endif
}
#ifndef TIMECOUNTER_TEST
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE mutex = nullptr; int result = 1;
    try {
        auto name = tc::mutexName(); mutex = CreateMutexW(nullptr, TRUE, name.c_str()); tc::check(mutex != nullptr, "Single instance mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            DWORD wait = WaitForSingleObject(mutex, 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { MessageBoxW(nullptr, L"TimeCounter уже запущен для этого пользователя.", L"TimeCounter", MB_OK | MB_ICONINFORMATION); CloseHandle(mutex); return 0; }
        }
        tc::App app(tc::Settings::load(), tc::dataDirectory() + L"\\history.json"); result = app.run();
    } catch (const std::exception& e) { tc::logError(e.what()); MessageBoxW(nullptr, tc::wide(e.what()).c_str(), L"TimeCounter: ошибка запуска", MB_OK | MB_ICONERROR); }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); } return result;
}
#endif
