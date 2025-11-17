// ThunderzEditor.cpp (Clean Stable Build)

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

using namespace Gdiplus;
namespace fs = std::filesystem;
#pragma comment(lib, "gdiplus.lib")

int gW = 1000, gH = 700;
ULONG_PTR gToken;

struct Entity {
    std::wstring id;
    std::wstring asset;
    int x, y;
    int w, h;
    int layer;
};

std::vector<Entity> entities;
std::vector<std::wstring> assetFiles;
std::vector<Bitmap*> assetImages;
int selectedIndex = 0;
int draggingIndex = -1;
int dragX, dragY;

std::wstring assetsDir = L"..\\Projects\\ExampleProject\\assets\\";

void ScanAssets() {
    for (auto p : assetImages) if (p) delete p;
    assetFiles.clear();
    assetImages.clear();

    try {
        for (auto &p : fs::directory_iterator(assetsDir)) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().wstring();
            for (auto &c : ext) c = towlower(c);
            if (ext == L".png" || ext == L".jpg" || ext == L".bmp") {
                assetFiles.push_back(p.path().wstring());
            }
        }
    } catch (...) {}

    std::sort(assetFiles.begin(), assetFiles.end());

    for (auto &f : assetFiles) {
        Bitmap *b = Bitmap::FromFile(f.c_str());
        if (b && b->GetLastStatus() == Ok) assetImages.push_back(b);
        else assetImages.push_back(nullptr);
    }

    if (selectedIndex >= (int)assetFiles.size()) selectedIndex = 0;
}

void SaveScene() {
    std::wstringstream ss;
    ss << L"{\n  \"id\":\"main\",\n  \"entities\":[\n";
    for (size_t i = 0; i < entities.size(); i++) {
        auto &e = entities[i];
        ss << L"    {\"id\":\"" << e.id << L"\",\"asset\":\"" << e.asset
           << L"\",\"x\":" << e.x << L",\"y\":" << e.y
           << L",\"layer\":" << e.layer << L"}";
        if (i + 1 < entities.size()) ss << L",";
        ss << L"\n";
    }
    ss << L"  ]\n}\n";

    std::wstring out = ss.str();
    int len = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, utf8.data(), len, NULL, NULL);

    std::ofstream f("..\\Projects\\ExampleProject\\scenes\\main.scene", std::ios::binary);
    f << utf8;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam), my = HIWORD(lParam);

        // CHECK IF CLICKING THUMBNAIL
        int left = gW - 150;
        int top = 10;
        for (size_t i = 0; i < assetImages.size(); i++) {
            RECT r = { left, top + (int)i * 80, left + 120, top + (int)i * 80 + 70 };
            if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom) {
                selectedIndex = (int)i;
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
        }

        // CHECK DRAG START
        for (int i = (int)entities.size() - 1; i >= 0; i--) {
            auto &e = entities[i];
            RECT r = { e.x, e.y, e.x + e.w, e.y + e.h };
            if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom) {
                draggingIndex = i;
                dragX = mx - e.x;
                dragY = my - e.y;
                return 0;
            }
        }

        // PLACE NEW ENTITY
        if (!assetImages.empty() && selectedIndex < (int)assetImages.size()) {
            Entity e;
            e.asset = fs::path(assetFiles[selectedIndex]).filename().wstring();
            e.id = e.asset + std::to_wstring(entities.size() + 1);
            e.x = mx;
            e.y = my;
            Bitmap *b = assetImages[selectedIndex];
            e.w = b ? b->GetWidth() : 64;
            e.h = b ? b->GetHeight() : 64;
            e.layer = 1;
            entities.push_back(e);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (draggingIndex >= 0) {
            int mx = LOWORD(lParam), my = HIWORD(lParam);
            entities[draggingIndex].x = mx - dragX;
            entities[draggingIndex].y = my - dragY;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        draggingIndex = -1;
        return 0;

    case WM_KEYDOWN:
        if (wParam == 'S') SaveScene();
        if (wParam == VK_LEFT && selectedIndex > 0) selectedIndex--;
        if (wParam == VK_RIGHT && selectedIndex + 1 < (int)assetFiles.size()) selectedIndex++;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Graphics g(dc);

        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.Clear(Color(30, 30, 30));

        // DRAW SCENE ENTITIES
        for (auto &e : entities) {
            for (size_t i = 0; i < assetFiles.size(); i++) {
                if (fs::path(assetFiles[i]).filename() == e.asset && assetImages[i]) {
                    g.DrawImage(assetImages[i], e.x, e.y, e.w, e.h);
                }
            }
        }

        // RIGHT PANEL
        SolidBrush panel(Color(180, 20, 20, 20));
        g.FillRectangle(&panel, gW - 150, 0, 150, gH);

        int y = 10;
        for (size_t i = 0; i < assetImages.size(); i++) {
            SolidBrush bg(i == selectedIndex ? Color(255, 120, 120, 40) : Color(255, 40, 40, 40));
            Rect r(gW - 140, y, 120, 70);
            g.FillRectangle(&bg, r);

            if (assetImages[i]) {
                Bitmap *b = assetImages[i];
                int iw = b->GetWidth(), ih = b->GetHeight();

                float sx = (float)r.Width / iw;
                float sy = (float)r.Height / ih;
                float scale = (sx < sy) ? sx : sy;

                int dw = (int)(iw * scale);
                int dh = (int)(ih * scale);
                int dx = r.X + (r.Width - dw) / 2;
                int dy = r.Y + (r.Height - dh) / 2;

                g.DrawImage(b, dx, dy, dw, dh);
            }
            y += 80;
        }

        EndPaint(h, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(h, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int nCmdShow) {
    GdiplusStartupInput gsi; 
    GdiplusStartup(&gToken, &gsi, NULL);

    ScanAssets();

    WNDCLASS wc = { };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = h;
    wc.lpszClassName = L"ThunderzEditor";

    RegisterClass(&wc);

    HWND win = CreateWindow(wc.lpszClassName, L"Thunderz Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, gW, gH,
        NULL, NULL, h, NULL);

    ShowWindow(win, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    for (auto p : assetImages) if (p) delete p;
    GdiplusShutdown(gToken);
    return 0;
}

