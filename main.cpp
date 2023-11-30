#ifndef UNICODE
#  define UNICODE
#endif

#include <initguid.h>
#include <windows.h>
#include <winnt.h>
#include <guiddef.h>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#ifdef NO_CONSOLE
#  define logErr(...)
#  define getLastErrorMessage()
#else
#  include <iostream>
#  include <format>
#  include <io.h>  // _setmode
#  include <fcntl.h>  // _O_U16TEXT
template <class... Args>
void logErr(std::wformat_string<Args...> fmt, Args&&... args) {
    std::wcerr << std::format(fmt, std::forward<Args>(args)...) << std::endl;
}

std::wstring getLastErrorMessage() {
    struct AutoBuf {
        AutoBuf() : ptr(nullptr) {}
        ~AutoBuf() {LocalFree(ptr);};
        LPVOID ptr;
    };

    constexpr DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    // constexpr DWORD langID = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    constexpr DWORD langID = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

    DWORD errcode = GetLastError();
    AutoBuf buf;

    auto nChar = FormatMessageW(flags, nullptr, errcode,
            langID, reinterpret_cast<LPWSTR>(&buf.ptr), 0, nullptr);

    if (nChar == 0) {
        logErr(L"Internal error: FormatMessage() failed.");
        return L"";
    } else if (buf.ptr == nullptr) {
        return L"";
    }

    const LPWSTR m = reinterpret_cast<LPWSTR>(buf.ptr);
    while (m[nChar-1] == '\n' || m[nChar-1] == '\r')
        nChar--;

    return std::wstring(reinterpret_cast<LPWSTR>(buf.ptr), nChar);
}
#endif

namespace BatteryFlag {
constexpr BYTE High = 1;
constexpr BYTE Low = 2;
constexpr BYTE Critical = 4;
constexpr BYTE Charging = 8;
constexpr BYTE NoSystemBattery = 128;
constexpr BYTE UnknownStatus = 255;
}

struct BatteryStatus {
    bool isCharging;  // TRUE if battery is charging.
    BYTE percent;  // How much energy remains.
};

class Tasktray {
public:
    Tasktray();
    ~Tasktray();

    Tasktray(const Tasktray&) = delete;
    Tasktray(Tasktray&&) = default;
    Tasktray& operator=(const Tasktray&) = delete;
    Tasktray& operator=(Tasktray&&) = default;
private:
    static LRESULT CALLBACK windowProc(
            HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool setup();
    void terminate();
    void showMenu();
    void checkBatteryStatus();
private:
    static constexpr std::wstring_view className{L"btwatch-class"};
    static constexpr UINT tasktrayIconID = 100;
    static constexpr UINT WM_TASKTRAY_CALLBACK = WM_APP + 1;

    HWND hwnd_;
    HPOWERNOTIFY hpn_;
    std::vector<HPOWERNOTIFY> hpns_;
    std::optional<ATOM> wndAtom_;
    BatteryStatus prevBtStatus_;
};

Tasktray::Tasktray()
    : hwnd_{}, hpn_{}, wndAtom_{}, prevBtStatus_{}
{
    if (!setup()) {
        terminate();
    }
}

Tasktray::~Tasktray() {
    terminate();
}

LRESULT CALLBACK Tasktray::windowProc(
    HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    Tasktray *pThis = NULL;

    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT *pCreate =
            reinterpret_cast<CREATESTRUCT *>(lParam);
        pThis = static_cast<Tasktray *>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);

        pThis->hwnd_ = hwnd;
    } else {
        pThis = reinterpret_cast<Tasktray *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handleMessage(uMsg, wParam, lParam);
    } else {
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

LRESULT Tasktray::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CLOSE:
        terminate();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_TASKTRAY_CALLBACK:
        if (wParam == tasktrayIconID) {
            if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP) {
                showMenu();
            }
        }
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_POWERSETTINGCHANGE) {
            auto setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(lParam);
            if (IsEqualGUID(setting->PowerSetting, GUID_BATTERY_PERCENTAGE_REMAINING)) {
                checkBatteryStatus();
            }
            if (IsEqualGUID(setting->PowerSetting, GUID_ACDC_POWER_SOURCE)) {
                checkBatteryStatus();
            }
        }
        break;  // Fallthrough this message to DefWindowProcW.
    }
    return DefWindowProcW(hwnd_, uMsg, wParam, lParam);
}

bool Tasktray::setup() {
    // Create window.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Tasktray::windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className.data();
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); // TODO: Set proper icon
    if (!wc.hIcon) {
        logErr(L"LoadIconW(): {}", getLastErrorMessage());
    }
    wndAtom_ = RegisterClassExW(&wc);
    if (wndAtom_.value() == 0) {
        logErr(L"RegisterClassExW(): {}", getLastErrorMessage());
        return false;
    }

    constexpr DWORD dwExStyle = 0;
    constexpr DWORD dwStyle = WS_TILEDWINDOW;
    hwnd_ = CreateWindowExW(
            dwExStyle, className.data(), L"btwatch", dwStyle,
            0, 0, 0, 0,
            0, 0, GetModuleHandleW(nullptr), this);

    if (!hwnd_) {
        logErr(L"CreateWindowExW(): {}", getLastErrorMessage());
        return false;
    }
    // ShowWindow(hwnd_, SW_SHOW);

    // Add tasktray
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.uID = tasktrayIconID;
    nid.hWnd = hwnd_;
    nid.hIcon = LoadIconW(nullptr, IDI_WINLOGO); // TODO: Set proper icon
    if (!nid.hIcon) {
        logErr(L"LoadIconW(): {}", getLastErrorMessage());
    }
    nid.uVersion = NOTIFYICON_VERSION;
    nid.uCallbackMessage = WM_TASKTRAY_CALLBACK;
    wcscpy_s(nid.szTip, sizeof(nid.szTip), L"btwatch");
    nid.uFlags = NIF_TIP | NIF_ICON | NIF_MESSAGE;
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        logErr(L"Shell_NotifyIconW(NIM_ADD): {}", getLastErrorMessage());
        return false;
    }

    // Register power setting notification
    const auto guids = {
        GUID_BATTERY_PERCENTAGE_REMAINING,
        GUID_ACDC_POWER_SOURCE,
    };
    for (const auto& guid : guids) {
        HPOWERNOTIFY hpn = RegisterPowerSettingNotification(
                hwnd_, &guid, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!hpn) {
            logErr(L"RegisterPowerSettingNotification(): {}", getLastErrorMessage());
            return false;
        }
        hpns_.push_back(hpn);
    }

    return true;
}

void Tasktray::terminate() {
    // Unregister power change notification
    if (!hpns_.empty()) {
        for (const auto& hpn : hpns_) {
            if (!UnregisterPowerSettingNotification(hpn)) {
                logErr(L"UnregisterPowerSettingNotification(): {}",
                        getLastErrorMessage());
            }
        }
        hpns_.clear();
    }

    // Remove application icon from tasktray.
    // Do this only when Window is still valid.
    if (hwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = tasktrayIconID;
        if (!Shell_NotifyIconW(NIM_DELETE, &nid)) {
            logErr(L"Shell_NotifyIconW(NIM_DELETE): {}", getLastErrorMessage());
        }
    }

    // Close window.
    if (hwnd_) {
        if (!DestroyWindow(hwnd_)) {
            logErr(L"DestroyWindow(): {}", getLastErrorMessage());
        }
        hwnd_ = nullptr;
    }

    // Unregister window class.
    if (wndAtom_.has_value()) {
        const auto ret = UnregisterClassW(
                reinterpret_cast<LPCWSTR>(wndAtom_.value()),
                GetModuleHandleW(nullptr));
        if (ret == 0) {
            logErr(L"UnregisterClassW(): {}", getLastErrorMessage());
        }
        wndAtom_ = std::nullopt;
    }
}

void Tasktray::showMenu() {
    enum class Cmd { None, Quit };
    constexpr UINT menuFlags =
        TPM_NONOTIFY | TPM_RETURNCMD |
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;

    POINT point;
    if (!GetCursorPos(&point)) {
        logErr(L"GetCursorPos() failed. Cancel open menu: {}",
                getLastErrorMessage());
        return;
    }

    HMENU hmenu = CreatePopupMenu();
    if (!hmenu) {
        logErr(L"CreatePopupMenu(): {}", getLastErrorMessage());
        return;
    }

    AppendMenuW(hmenu, MF_STRING, std::to_underlying(Cmd::Quit), L"&Quit");

    const HWND prevWin = GetActiveWindow();
    SetForegroundWindow(hwnd_);
    const auto op = TrackPopupMenuEx(
            hmenu, menuFlags, point.x, point.y, hwnd_, nullptr);

    switch (op) {
    case std::to_underlying(Cmd::None):
        // Menu is closed with any action.  Do nothing.
        break;
    case std::to_underlying(Cmd::Quit):
        terminate();
        break;
    default:
        std::unreachable();
    }

    DestroyMenu(hmenu);
    if (prevWin) {
        SetActiveWindow(prevWin);
    }
}

void Tasktray::checkBatteryStatus() {
    constexpr BYTE BatteryLifePercentUnknown = 255;

    SYSTEM_POWER_STATUS status{};
    GetSystemPowerStatus(&status);

    if (status.BatteryLifePercent == BatteryLifePercentUnknown) {
        return;
    }

    const bool isCharging = status.BatteryFlag & BatteryFlag::Charging;

    if (status.BatteryLifePercent >= 90 && isCharging &&
            (!prevBtStatus_.isCharging || prevBtStatus_.percent < 90)) {
        NOTIFYICONDATA nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = tasktrayIconID;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcscpy_s(nid.szInfoTitle, sizeof(nid.szInfoTitle),
                L"The battery charge has exceeded 90%.");
        wcscpy_s(nid.szInfo, sizeof(nid.szInfo),
                L"Probably now it's time to unplug the battery charger.");

#ifndef NO_CONSOLE
        std::wcout << L"The battery charge has exceeded 90%." << std::endl;
#endif

        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            logErr(L"Shell_NotifyIconW(NIM_MODIFY): {}", getLastErrorMessage());
        }
    }

    prevBtStatus_.percent = status.BatteryLifePercent;
    prevBtStatus_.isCharging = isCharging;
}

int WINAPI wWinMain(
        HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)pCmdLine;
    (void)nCmdShow;

#ifndef NO_CONSOLE
    // Enable UTF-16 in console output.
    ::_setmode(_fileno(stdout), _O_U16TEXT);
    ::_setmode(_fileno(stderr), _O_U16TEXT);
#endif

    SYSTEM_POWER_STATUS status{};
    GetSystemPowerStatus(&status);
    if (status.BatteryFlag & BatteryFlag::NoSystemBattery) {
        logErr(L"It seems this computer does not work by battery. Quit.");
        return 0;
    }

    Tasktray tasktray;

    MSG msg{};
    for (;;) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {
            break;
        } else if (r == -1) {
            logErr(L"GetMessageW(): {}", getLastErrorMessage());
            break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
