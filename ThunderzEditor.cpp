// ThunderzEditor.cpp (asset enumeration + selection)
// Compile with -std=gnu++17

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

int gW = 800, gH = 600;
ULONG_PTR gToken;

struct Entity { std::wstring id; std::wstring asset; int x,y; int layer; };
std::vector<Entity> entities;

std::vector<std::wstring> assetFiles; // full paths (wstrings)
std::vector<Bitmap*> assetImages;      // loaded bitmaps
int selectedIndex = 0;

std::wstring assetsDir = L"..\\Projects\\ExampleProject\\assets\\";

std::wstring to_wstring_from_utf8(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}

void ScanAssets() {
    assetFiles.clear();
    assetImages.clear();
    try {
        for (auto &p : fs::directory_iterator(std::wstring(assetsDir))) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".png" || ext == L".bmp" || ext == L".jpg" || ext == L".jpeg") {
                std::wstring full = p.path().wstring();
                assetFiles.push_back(full);
            }
        }
    } catch (...) {
        // ignore
    }
    // sort by filename
    std::sort(assetFiles.begin(), assetFiles.end());
    for (auto &f : assetFiles) {
        Bitmap* b = Bitmap::FromFile(f.c_str());
        if (b && b->GetLastStatus() == Ok) assetImages.push_back(b);
        else assetImages.push_back(nullptr);
    }
    if (selectedIndex >= (int)assetFiles.size()) selectedIndex = (int)assetFiles.size() ? 0 : -1;
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
    case WM_CREATE:
        return 0;
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        if (selectedIndex >= 0 && selectedIndex < (int)assetFiles.size()) {
            Entity e;
            // make id from filename
            std::wstring fname = fs::path(assetFiles[selectedIndex]).filename().wstring();
            e.id = fname + std::to_wstring(entities.size()+1);
            e.asset = fname; e.x = x; e.y = y; e.layer = 1;
            entities.push_back(e);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (!entities.empty()) { entities.pop_back(); InvalidateRect(hWnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_RIGHT) { if (!assetFiles.empty()) selectedIndex = (selectedIndex+1) % assetFiles.size(); }
        if (wParam == VK_LEFT)  { if (!assetFiles.empty()) selectedIndex = (selectedIndex-1 + assetFiles.size()) % assetFiles.size(); }
        if (wParam >= '1' && wParam <= '9') {
            int idx = (wParam - '1'); if (idx < (int)assetFiles.size()) selectedIndex = idx;
        }
        if (wParam == 'S') SaveSceneJSON(L"..\\Projects\\ExampleProject\\scenes\\main.scene");
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.Clear(Color(60,130,200));
        // draw full background if first asset is large (we still draw bg as first asset if exists)
        if (!assetImages.empty() && assetImages[0]) {
            g.DrawImage(assetImages[0], 0, 0, gW, gH);
        }
        // draw entities
        for (auto &e : entities) {
            // find index of this asset by name
            for (size_t i=0;i<assetFiles.size();++i) {
                std::wstring fname = fs::path(assetFiles[i]).filename().wstring();
                if (fname == e.asset) {
                    if (assetImages[i]) g.DrawImage(assetImages[i], e.x, e.y);
                    break;
                }
            }
        }
        // UI text
        SolidBrush brush(Color(255,255,255,255));
        FontFamily ff(L"Arial"); Font font(&ff, 14, FontStyleRegular, UnitPixel);
        std::wstring info = L"Thunderz Editor - ArrowLeft/Right to change asset, 1..9 direct select, LeftClick place, RightClick undo, S=save";
        g.DrawString(info.c_str(), -1, &font, PointF(8,8), &brush);
        // selected asset name
        std::wstring sel = L"Selected: ";
        if (selectedIndex >= 0 && selectedIndex < (int)assetFiles.size()) {
            sel += fs::path(assetFiles[selectedIndex]).filename().wstring();
        } else sel += L"(none)";
        g.DrawString(sel.c_str(), -1, &font, PointF(8,32), &brush);
        // draw thumbnails small on right
        int thumbX = gW - 120, thumbY = 8, thumbH = 48;
        for (size_t i=0;i<assetImages.size();++i) {
            Rect dst(thumbX, thumbY + (int)i*(thumbH+6), 100, thumbH);
            if (assetImages[i]) g.DrawImage(assetImages[i], dst);
            // highlight border if selected
            if ((int)i == selectedIndex) {
                Pen pen(Color(255,255,0,0), 2);
                g.DrawRectangle(&pen, dst);
            }
        }
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
    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Editor", WS_OVERLAPPEDWINDOW ^ WS_SIZEBOX,
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
