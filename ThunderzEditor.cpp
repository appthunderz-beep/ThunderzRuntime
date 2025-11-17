// ThunderzEditor.cpp
// Minimal Thunderz Editor (Win32 + GDI+). Compile with same workflow as runtime.
// Note: small UNICODE warning is harmless; keep as-is.

#define UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

using namespace Gdiplus;
#pragma comment(lib, "gdiplus.lib")

int gW = 800, gH = 600;
ULONG_PTR gToken;

struct Entity {
    std::wstring id;
    std::wstring asset;
    int x, y;
    int layer;
};

std::vector<Entity> entities;
Bitmap* bmpBg = nullptr;
Bitmap* bmpHero = nullptr;
int selectedAsset = 1; // 1=bg,2=hero

void SaveSceneJSON(const std::wstring &projectPath) {
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
    std::ofstream ofs(std::string(projectPath.begin(), projectPath.end()), std::ios::binary);
    ofs << out;
    ofs.close();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        Entity e;
        e.x = x; e.y = y; e.layer = (selectedAsset==1?0:1);
        e.asset = (selectedAsset==1?L"bg_main":L"hero");
        e.id = e.asset + std::to_wstring(entities.size()+1);
        entities.push_back(e);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (!entities.empty()) { entities.pop_back(); InvalidateRect(hWnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wParam == '1') selectedAsset = 1;
        if (wParam == '2') selectedAsset = 2;
        if (wParam == 'S') {
            SaveSceneJSON(L"..\\Projects\\ExampleProject\\scenes\\main.scene");
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.Clear(Color(60,130,200));
        if (bmpBg) g.DrawImage(bmpBg, 0, 0, gW, gH);
        for (auto &e : entities) {
            if (e.asset==L"hero" && bmpHero) g.DrawImage(bmpHero, e.x, e.y);
        }
        SolidBrush brush(Color(255,255,255,255));
        FontFamily ff(L"Arial"); Font font(&ff, 14, FontStyleRegular, UnitPixel);
        std::wstring info = L"Thunderz Editor - 1=BG  2=Hero  LeftClick=place  RightClick=undo  S=save scene";
        g.DrawString(info.c_str(), -1, &font, PointF(8,8), &brush);
        std::wstring sel = (selectedAsset==1?L"Selected: BG":L"Selected: HERO");
        g.DrawString(sel.c_str(), -1, &font, PointF(8,32), &brush);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow){
    GdiplusStartupInput gsi; GdiplusStartup(&gToken, &gsi, NULL);
    bmpBg = Bitmap::FromFile(L"..\\Projects\\ExampleProject\\assets\\bg.bmp");
    bmpHero = Bitmap::FromFile(L"..\\Projects\\ExampleProject\\assets\\hero.bmp");
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance; wc.lpszClassName = L"ThunderzEditor";
    RegisterClass(&wc);
    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Editor", WS_OVERLAPPEDWINDOW ^ WS_SIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, gW, gH, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (bmpBg) delete bmpBg; if (bmpHero) delete bmpHero;
    GdiplusShutdown(gToken);
    return 0;
}
