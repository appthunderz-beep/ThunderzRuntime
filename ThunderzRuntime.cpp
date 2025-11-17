// ThunderzRuntime.cpp
// Minimal Thunderz runtime (Win32 + GDI+). 64-bit build recommended.
// Compile (x64): cl /EHsc /MD ThunderzRuntime.cpp /link gdiplus.lib user32.lib gdi32.lib kernel32.lib

#define UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>

using namespace Gdiplus;
#pragma comment (lib,"gdiplus.lib")

int gW = 800, gH = 600;
ULONG_PTR gdiplusToken;

struct Img { std::wstring path; Bitmap* bmp = nullptr; };
std::vector<Img> imgs;

Bitmap* LoadBmp(const std::wstring &p) {
    Bitmap* b = Bitmap::FromFile(p.c_str());
    if (b && b->GetLastStatus() == Ok) return b;
    if (b) { delete b; return nullptr; }
    return nullptr;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static float heroX = 100.f, heroY = 300.f;
    static DWORD last = GetTickCount();
    switch (msg) {
    case WM_CREATE: last = GetTickCount(); return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) { MessageBox(hWnd, L"Thunderz: Demo dialog", L"Thunderz", MB_OK); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.Clear(Color(100,150,200));
        if (imgs.size() > 0 && imgs[0].bmp) g.DrawImage(imgs[0].bmp, 0, 0, gW, gH);
        if (imgs.size() > 1 && imgs[1].bmp) g.DrawImage(imgs[1].bmp, (INT)heroX, (INT)heroY);
        SolidBrush brush(Color(255,255,255,255));
        FontFamily ff(L"Arial"); Font font(&ff, 14, FontStyleRegular, UnitPixel);
        g.DrawString(L"Thunderz Runtime - Left/Right move (hold), Space dialog", -1, &font, PointF(10,10), &brush);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void RunLoop(HWND hWnd) {
    MSG msg; DWORD last = GetTickCount();
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
    GdiplusStartupInput gsi; GdiplusStartup(&gdiplusToken, &gsi, NULL);
    imgs.push_back({L"assets\\bg.bmp", nullptr});
    imgs.push_back({L"assets\\hero.bmp", nullptr});
    for (auto &i : imgs) i.bmp = LoadBmp(i.path);

    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = L"THUNDERZ";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClass(&wc);
    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Runtime", WS_OVERLAPPEDWINDOW ^ WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, gW, gH, NULL, NULL, hInstance, NULL);
    if (!hWnd) { MessageBox(NULL, L"CreateWindow failed", L"Error", MB_OK); return -1; }
    ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);
    RunLoop(hWnd);
    for (auto &i : imgs) if (i.bmp) delete i.bmp;
    GdiplusShutdown(gdiplusToken);
    return 0;
}
