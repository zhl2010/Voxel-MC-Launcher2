// 主窗口：无边框自绘标题栏
//
// 为什么用 WS_POPUP 而不是 WS_OVERLAPPEDWINDOW + 去掉 WS_CAPTION：
// 前者从根上没有非客户区，不会出现「系统标题栏的影子/边框残留」，
// 与旧 Electron 版 frame:false 的观感一致。前端 styles.css 里
// -webkit-app-region: drag 的拖拽区由 WebView2 层单独处理（webview.cpp）。

#pragma once

#include <windows.h>
#include <functional>

class MainWindow {
public:
    // 客户区尺寸变化回调（单位：像素）
    using ResizeHandler = std::function<void(int width, int height)>;

    bool Create(HINSTANCE inst, const wchar_t* title, int width, int height, COLORREF bg);
    void Show(int cmdShow);
    void SetMinimumSize(int w, int h);
    void SetResizeHandler(ResizeHandler handler);
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HBRUSH brush_ = nullptr;
    int minW_ = 0;
    int minH_ = 0;
    ResizeHandler onResize_;
};
