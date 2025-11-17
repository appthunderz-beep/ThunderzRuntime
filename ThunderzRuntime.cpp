// ThunderzRuntime.cpp
// Improved visuals: smoothing, drop shadow, scaled PNG support.

#define UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>

using namespace Gdiplus;
#pragma comment (lib,"gdiplus.lib")

int gW = 800, gH = 600;
ULONG_PTR gdiplusToken;

struct Img { std::wstring path; Image* img = nullptr; int w=0,h=0; };
std::vector<Img> imgs;

Image* LoadAny(const std::wstring &p) {
    Image* im = Image::FromFile(p.c_str());
    if (im && im->GetLastStatus() == Ok) return im;
    if (im) { delete im; return nullptr; }
    return nullptr;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static float heroX = 100.f, heroY = 300.f;
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) MessageBox(hWnd, L"Thunderz: Demo dialog", L"Thunderz", MB_OK);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        g.Clear(Color(100,150,200));
        if (imgs.size()>0 && imgs[0].img) g.DrawImage(imgs[0].img, 0, 0, gW, gH);

        // draw hero with drop shadow and scale
        if (imgs.size()>1 && imgs[1].img) {
            int iw = imgs[1].w, ih = imgs[1].h;
            float scale = 1.0f;
            int dw = (int)(iw*scale), dh = (int)(ih*scale);

            // shadow
            SolidBrush shadow(Color(140,0,0,0));
            g.FillRectangle(&shadow, (INT)heroX+8, (INT)heroY+8, dw, dh);
            g.DrawImage(imgs[1].img, (INT)heroX, (INT)heroY, dw, dh);
        }

        FontFamily ff(L"Segoe UI"); Font font(&ff, 14, FontStyleRegular, UnitPixel);
        SolidBrush brush(Color(255,255,255,255));
        g.DrawString(L"Thunderz Runtime - Arrow keys move, Space dialog", -1, &font, PointF(10,10), &brush);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProc(hWnd,msg,wParam,lParam);
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
    // try PNG first, then bmp
    std::wstring tries[] = { L"assets\\bg.png", L"assets\\hero.png", L"assets\\bg.bmp", L"assets\\hero.bmp" };
    for (auto &p : tries) {
        Img im; im.path = p; im.img = LoadAny(p);
        if (im.img) { im.w = im.img->GetWidth(); im.h = im.img->GetHeight(); imgs.push_back(im); }
    }

    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = L"THUNDERZ";
    RegisterClass(&wc);
    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Runtime", WS_OVERLAPPEDWINDOW ^ WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, gW, gH, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);
    RunLoop(hWnd);
    for (auto &i: imgs) if (i.img) delete i.img;
    GdiplusShutdown(gdiplusToken);
    return 0;
}

