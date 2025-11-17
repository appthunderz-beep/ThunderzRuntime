// ThunderzEditor.cpp
// Improved visuals: smoothing, grid, thumbnails with background, drag-to-move, rounded UI panels.
// Compile with -std=gnu++17 (workflow already set).

#define UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

using namespace Gdiplus;
namespace fs = std::filesystem;
#pragma comment(lib, "gdiplus.lib")

int gW = 1000, gH = 700;
ULONG_PTR gToken;

struct Entity { std::wstring id; std::wstring asset; int x,y; int layer; int w,h; };
std::vector<Entity> entities;

std::vector<std::wstring> assetFiles;
std::vector<Bitmap*> assetImages;
int selectedIndex = -1;
int draggingIndex = -1;
int dragOffX=0, dragOffY=0;

std::wstring assetsDir = L"..\\Projects\\ExampleProject\\assets\\";

void ScanAssets() {
    for (auto b : assetImages) if (b) delete b;
    assetFiles.clear(); assetImages.clear();
    try {
        for (auto &p : fs::directory_iterator(std::wstring(assetsDir))) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".png" || ext == L".bmp" || ext == L".jpg" || ext == L".jpeg") {
                assetFiles.push_back(p.path().wstring());
            }
        }
    } catch (...) {}
    std::sort(assetFiles.begin(), assetFiles.end());
    for (auto &f : assetFiles) {
        Bitmap* b = Bitmap::FromFile(f.c_str());
        if (b && b->GetLastStatus() == Ok) assetImages.push_back(b);
        else assetImages.push_back(nullptr);
    }
    if (!assetFiles.empty() && selectedIndex < 0) selectedIndex = 0;
}

void SaveSceneJSON(const std::wstring &path) {
    std::wstringstream ss;
    ss << L"{\n  \"id\":\"main\",\n  \"entities\":[\n";
    for (size_t i=0;i<entities.size();++i){
        auto &e = entities[i];
        ss << L"    {\"id\":\"" << e.id << L"\",\"asset\":\"" << e.asset << L"\",\"x\":" << e.x << L",\"y\":"<<e.y<<L",\"layer\":"<<e.layer<<L"}";
        if (i+1<entities.size()) ss << L",";
        ss << L"\n";
    }
    ss << L"  ],\n  \"script\":[]\n}\n";
    std::wstring ws = ss.str();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    std::string out(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &out[0], size_needed, NULL, NULL);
    std::ofstream ofs(std::string(path.begin(), path.end()), std::ios::binary);
    ofs << out; ofs.close();
}

// helper: draw rounded rect
void DrawRoundedRect(Graphics& g, const Rect& r, int radius, const Pen& pen) {
    GraphicsPath path;
    path.AddArc(r.GetLeft(), r.GetTop(), radius, radius, 180, 90);
    path.AddArc(r.GetRight()-radius, r.GetTop(), radius, radius, 270, 90);
    path.AddArc(r.GetRight()-radius, r.GetBottom()-radius, radius, radius, 0, 90);
    path.AddArc(r.GetLeft(), r.GetBottom()-radius, radius, radius, 90, 90);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool mouseDown = false;
    switch(msg) {
    case WM_CREATE:
        return 0;
    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        // check thumbnails area for asset click first (right side)
        int thumbX = gW - 140;
        int thumbY = 12;
        int thumbH = 64;
        for (size_t i=0;i<assetImages.size();++i) {
            Rect dst(thumbX, thumbY + (int)i*(thumbH+8), 120, thumbH);
            if (mx>=dst.GetLeft() && mx<=dst.GetRight() && my>=dst.GetTop() && my<=dst.GetBottom()) {
                selectedIndex = (int)i;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }
        // check if clicking an existing entity to start drag
        for (int i=(int)entities.size()-1;i>=0;--i) {
            Entity &e = entities[i];
            Rect er(e.x, e.y, e.w, e.h);
            if (mx>=er.GetLeft() && mx<=er.GetRight() && my>=er.GetTop() && my<=er.GetBottom()) {
                draggingIndex = i;
                dragOffX = mx - e.x; dragOffY = my - e.y;
                mouseDown = true;
                return 0;
            }
        }
        // else place selected asset
        if (selectedIndex >=0 && selectedIndex < (int)assetFiles.size()) {
            Entity e;
            std::wstring fname = fs::path(assetFiles[selectedIndex]).filename().wstring();
            e.id = fname + std::to_wstring(entities.size()+1);
            e.asset = fname;
            e.x = mx; e.y = my; e.layer = 1;
            if (assetImages[selectedIndex]) { e.w = assetImages[selectedIndex]->GetWidth(); e.h = assetImages[selectedIndex]->GetHeight(); }
            else { e.w = 64; e.h = 64; }
            entities.push_back(e);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (mouseDown && draggingIndex >=0) {
            int mx = LOWORD(lParam), my = HIWORD(lParam);
            entities[draggingIndex].x = mx - dragOffX;
            entities[draggingIndex].y = my - dragOffY;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        mouseDown = false; draggingIndex = -1;
        return 0;
    case WM_RBUTTONDOWN:
        if (!entities.empty()) { entities.pop_back(); InvalidateRect(hWnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_RIGHT) { if (!assetFiles.empty()) selectedIndex = (selectedIndex+1) % assetFiles.size(); }
        if (wParam == VK_LEFT)  { if (!assetFiles.empty()) selectedIndex = (selectedIndex-1 + assetFiles.size()) % assetFiles.size(); }
        if (wParam >= '1' && wParam <= '9') { int idx = (wParam - '1'); if (idx < (int)assetFiles.size()) selectedIndex = idx; }
        if (wParam == 'S') SaveSceneJSON(L"..\\Projects\\ExampleProject\\scenes\\main.scene");
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        // high quality rendering
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // draw subtle grid
        SolidBrush gridBrush(Color(20,255,255,255));
        for (int gx=0; gx<gW; gx+=40) g.FillRectangle(&gridBrush, gx, 0, 1, gH);
        for (int gy=0; gy<gH; gy+=40) g.FillRectangle(&gridBrush, 0, gy, gW, 1);

        // draw background (if first asset is big)
        if (!assetImages.empty() && assetImages[0]) g.DrawImage(assetImages[0], 0, 0, gW, gH);

        // draw entities with subtle drop shadow
        for (auto &e : entities) {
            // find matching asset index
            int idx=-1;
            for (size_t i=0;i<assetFiles.size();++i) if (fs::path(assetFiles[i]).filename().wstring() == e.asset) { idx=(int)i; break; }
            if (idx>=0 && assetImages[idx]) {
                // drop shadow
                SolidBrush shadow(Color(120,0,0,0));
                g.FillRectangle(&shadow, e.x+6, e.y+6, e.w, e.h);
                g.DrawImage(assetImages[idx], e.x, e.y, e.w, e.h);
            } else {
                // fallback draw rectangle
                SolidBrush box(Color(200,80,80));
                g.FillRectangle(&box, e.x, e.y, e.w, e.h);
            }
        }

        // right side panel: thumbnails on slightly translucent rounded panel
        Rect panelRect(gW-150, 6, 140, gH-12);
        GraphicsPath p; p.AddRectangle(panelRect);
        SolidBrush panelFill(Color(150,20,20,20));
        g.FillPath(&panelFill, &p);

        // draw thumbnails
        int thumbX = gW - 140, thumbY = 12, thumbH = 64;
        for (size_t i=0;i<assetImages.size();++i) {
            Rect dst(thumbX, thumbY + (int)i*(thumbH+8), 120, thumbH);
            // draw small border/background
            SolidBrush bg(Color(200,40,40,40));
            g.FillRectangle(&bg, dst);
            if (assetImages[i]) {
                // fit thumbnail preserving ratio
                int iw = assetImages[i]->GetWidth(), ih = assetImages[i]->GetHeight();
                float scale = min( (float)dst.Width/iw, (float)dst.Height/ih );
                int dw = (int)(iw*scale), dh = (int)(ih*scale);
                int dx = dst.GetLeft() + (dst.Width-dw)/2, dy = dst.GetTop() + (dst.Height-dh)/2;
                g.DrawImage(assetImages[i], dx, dy, dw, dh);
            }
            // highlight selection
            if ((int)i == selectedIndex) {
                Pen p(Color(255,255,200,80), 3);
                g.DrawRectangle(&p, dst);
            }
        }

        // header text drawn with nicer font
        FontFamily ff(L"Segoe UI");
        Font header(&ff, 14, FontStyleBold, UnitPixel);
        SolidBrush textBrush(Color(255,255,255,255));
        g.DrawString(L"Thunderz Editor - ArrowLeft/Right to change asset, 1..9 select, LeftClick place, Drag to move, S=save", -1, &header, PointF(8,8), &textBrush);

        // selected asset label (bigger)
        Font selFont(&ff, 16, FontStyleBold, UnitPixel);
        std::wstring sel = L"Selected: ";
        if (selectedIndex>=0 && selectedIndex < (int)assetFiles.size()) sel += fs::path(assetFiles[selectedIndex]).filename().wstring();
        else sel += L"(none)";
        g.DrawString(sel.c_str(), -1, &selFont, PointF(8,30), &textBrush);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd,msg,wParam,lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    GdiplusStartupInput gsi; GdiplusStartup(&gToken, &gsi, NULL);
    ScanAssets();
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = L"ThunderzEditor";
    RegisterClass(&wc);
    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Editor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, gW, gH, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)){
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    for (auto b : assetImages) if (b) delete b;
    GdiplusShutdown(gToken);
    return 0;
}
