#include "window.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
constexpr wchar_t kClassName[] = L"VoxelLauncherMainWindow";
}

bool MainWindow::Create(HINSTANCE inst, const wchar_t* title, int width, int height, COLORREF bg) {
    brush_ = CreateSolidBrush(bg);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = brush_;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return false;

    // WS_EX_APPWINDOW：无边框窗口默认不进任务栏，显式要求有任务栏按钮
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW,
                            kClassName, title,
                            WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    // 圆角/阴影交给系统，避免无边框窗口看起来像一块生硬的矩形
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    return true;
}

void MainWindow::Show(int cmdShow) {
    // 旧版是启动即显示，不闪一下再亮；SW_SHOWDEFAULT 实测会先闪白
    ShowWindow(hwnd_, cmdShow == SW_SHOWDEFAULT ? SW_SHOWNORMAL : cmdShow);
    UpdateWindow(hwnd_);
}

void MainWindow::SetMinimumSize(int w, int h) {
    minW_ = w;
    minH_ = h;
}

void MainWindow::SetResizeHandler(ResizeHandler handler) {
    onResize_ = std::move(handler);
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            if (minW_ > 0 && minH_ > 0) {
                info->ptMinTrackSize.x = minW_;
                info->ptMinTrackSize.y = minH_;
            }
            return 0;
        }
        case WM_SIZE: {
            const int w = LOWORD(lp);
            const int h = HIWORD(lp);
            if (onResize_) onResize_(w, h);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, brush_);
            return 1;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
