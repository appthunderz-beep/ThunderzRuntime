// ThunderzRuntime.cpp
// Minimal Thunderz runtime (Win32 + GDI+). Loads images via GDI+ (PNG, BMP).
// Compile (x64): g++ ... -std=gnu++17 ... -municode

#define UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

using namespace Gdiplus;
#pragma comment (lib,"gdiplus.lib")

int gW = 800, gH = 600;
ULONG_PTR gdiplusToken;

struct ImageAsset {
    std::wstring id;
    std::wstring path;
    Image* image = nullptr;
};

std::vector<ImageAsset> images;

// Load arbitrary image file (BMP/PNG/JPEG supported by GDI+)
Image* LoadImageAny(const std::wstring &path) {
    Image* img = Image::FromFile(path.c_str());
    if (img && img->GetLastStatus() == Ok) return img;
    if (img) { delete img; return nullptr; }
    return nullptr;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static float heroX = 100.f, heroY = 300.f;
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            MessageBox(hWnd, L"Thunderz: Demo dialog", L"Thunderz", MB_OK);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.Clear(Color(100,150,200));
        // draw background (images[0]) and hero (images[1]) if present
        if (images.size() > 0 && images[0].image) {
            g.DrawImage(images[0].image, 0, 0, gW, gH);
        }
        if (images.size() > 1 && images[1].image) {
            g.DrawImage(images[1].image, (INT)heroX, (INT)heroY);
        }
        // debug text
        SolidBrush brush(Color(255,255,255,255));
        FontFamily fontFamily(L"Arial");
        Font font(&fontFamily, 14, FontStyleRegular, UnitPixel);
        PointF origin(10.0f, 10.0f);
        g.DrawString(L"Thunderz Runtime - Left/Right to move, Space dialog", -1, &font, origin, &brush);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

// Simple message loop reading async keys for movement
void RunMessageLoop(HWND hWnd) {
    MSG msg;
    DWORD last = GetTickCount();
    float heroX = 100.f;
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        DWORD now = GetTickCount(); float dt = (now - last) / 1000.f; last = now;
        if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0) heroX += 150.f * dt;
        if ((GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0) heroX -= 150.f * dt;
        InvalidateRect(hWnd, NULL, FALSE);
        Sleep(16);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL) != Ok) {
        MessageBox(NULL, L"Gdiplus init failed", L"Error", MB_OK);
        return -1;
    }

    // Try load images relative to working dir: first project assets or runtime/assets
    std::wstring paths[] = {
        L"assets\\bg.png",
        L"assets\\hero.png",
        L"assets\\bg.bmp",
        L"assets\\hero.bmp"
    };
    for (auto &p : paths) {
        ImageAsset ia;
        ia.path = p;
        ia.id = p;
        ia.image = LoadImageAny(p);
        if (ia.image) images.push_back(ia);
        // push only first two found, but continue to attempt both bg+hero
    }

    // Register window
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ThunderzWndClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Runtime", WS_OVERLAPPEDWINDOW ^ WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, gW, gH, NULL, NULL, hInstance, NULL);
    if (!hWnd) { MessageBox(NULL, L"CreateWindow failed", L"Error", MB_OK); return -1; }
    ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);
    RunMessageLoop(hWnd);

    for (auto &ia : images) if (ia.image) delete ia.image;
    GdiplusShutdown(gdiplusToken);
    return 0;
}
