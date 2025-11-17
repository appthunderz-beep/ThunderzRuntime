// ThunderzRuntime.cpp (Clean Stable Build)

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>

using namespace Gdiplus;
#pragma comment (lib, "gdiplus.lib")

ULONG_PTR gToken;

int gW = 800;
int gH = 600;

struct Img {
    std::wstring path;
    Image *bmp;
    int w, h;
};

std::vector<Img> imgs;

Image* LoadIMG(const std::wstring &p) {
    Image *i = Image::FromFile(p.c_str());
    if (i && i->GetLastStatus() == Ok) return i;
    if (i) delete i;
    return nullptr;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Graphics g(dc);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        g.Clear(Color(100, 130, 180));

        if (imgs.size() > 0 && imgs[0].bmp)
            g.DrawImage(imgs[0].bmp, 0, 0, gW, gH);

        if (imgs.size() > 1 && imgs[1].bmp) {
            int x = 300, y = 300;
            g.DrawImage(imgs[1].bmp, x, y, imgs[1].w, imgs[1].h);
        }

        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int cmd) {
    GdiplusStartupInput gsi;
    GdiplusStartup(&gToken, &gsi, NULL);

    std::vector<std::wstring> tries = {
        L"assets\\bg.png",
        L"assets\\hero.png",
        L"assets\\bg.bmp",
        L"assets\\hero.bmp"
    };

    for (auto &p : tries) {
        Img im;
        im.path = p;
        im.bmp = LoadIMG(p);
        if (im.bmp) {
            im.w = im.bmp->GetWidth();
            im.h = im.bmp->GetHeight();
            imgs.push_back(im);
        }
    }

    WNDCLASS wc = { };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = h;
    wc.lpszClassName = L"ThunderzRuntime";

    RegisterClass(&wc);

    HWND win = CreateWindow(wc.lpszClassName, L"Thunderz Runtime",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        gW, gH, NULL, NULL, h, NULL);

    ShowWindow(win, cmd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    for (auto &i : imgs) if (i.bmp) delete i.bmp;
    GdiplusShutdown(gToken);
    return 0;
}
