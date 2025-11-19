// ThunderzEditor.cpp - Full Fixed Version (GDI+ Win32 Editor)
// Phase 1: Scene View, Grid, Zoom, Pan, Place Sprites, Inspect Entity

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>  // <--- REQUIRED FOR GET_X_LPARAM, GET_Y_LPARAM
#include <gdiplus.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>

using namespace Gdiplus;
namespace fs = std::filesystem;

#pragma comment(lib, "gdiplus.lib")

// Window size
int winW = 1100, winH = 700;
int rightPanelW = 300;
int assetsPanelH = 140;
int toolbarH = 40;

ULONG_PTR gToken;

// ------------------- ENTITY STRUCT -------------------
struct Entity {
    std::wstring id;
    std::wstring assetFilename;
    int x, y;
    int w, h;
    int layer;
};

std::vector<Entity> entities;
std::vector<std::wstring> assetFiles;
std::vector<Bitmap*> assetImages;

int selectedAssetIndex = 0;
int selectedEntity = -1;

// Scene transform
float viewZoom = 1.0f;
float viewOffsetX = 0.0f, viewOffsetY = 0.0f;
bool panning = false;
int panStartX = 0, panStartY = 0;
float panStartOffsetX = 0, panStartOffsetY = 0;

// Dragging
bool dragging = false;
int dragEntity = -1;
int dragOffsetX = 0;
int dragOffsetY = 0;

// Paths
std::wstring projectAssetsDir = L"..\\Projects\\ExampleProject\\assets\\";
std::wstring sceneSavePath = L"..\\Projects\\ExampleProject\\scenes\\main.scene";

// --------------------- FILE HELPERS ---------------------
static std::string readAllBytes(const std::wstring& path) {
    std::ifstream ifs(std::string(path.begin(), path.end()), std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

// ------------------- ASSET SCANNER -------------------
void ScanAssets() {
    for (auto b : assetImages) if (b) delete b;
    assetImages.clear();
    assetFiles.clear();

    try {
        if (fs::exists(projectAssetsDir)) {
            for (auto& p : fs::directory_iterator(projectAssetsDir)) {
                if (!p.is_regular_file()) continue;

                auto ext = p.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), towlower);

                if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp")
                    assetFiles.push_back(p.path().wstring());
            }
        }
    } catch (...) {}

    std::sort(assetFiles.begin(), assetFiles.end());

    for (auto& path : assetFiles) {
        Bitmap* b = Bitmap::FromFile(path.c_str());
        if (b && b->GetLastStatus() == Ok) assetImages.push_back(b);
        else assetImages.push_back(nullptr);
    }

    if (selectedAssetIndex >= (int)assetFiles.size())
        selectedAssetIndex = std::max(0, (int)assetFiles.size() - 1);
}

// ------------------- SCENE SAVE -------------------
void SaveScene() {
    std::wstringstream ss;
    ss << L"{\n  \"entities\": [\n";

    for (size_t i = 0; i < entities.size(); i++) {
        Entity& e = entities[i];
        ss << L"    {\"asset\":\"" << e.assetFilename << L"\",\"x\":" << e.x
           << L",\"y\":" << e.y << L",\"w\":" << e.w << L",\"h\":" << e.h << L"}";
        if (i + 1 < entities.size()) ss << L",";
        ss << L"\n";
    }

    ss << L"  ]\n}";
    std::wstring ws = ss.str();

    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, utf8.data(), size, NULL, NULL);

    std::ofstream ofs(std::string(sceneSavePath.begin(), sceneSavePath.end()), std::ios::binary);
    ofs << utf8;
}

// ------------------- SCENE LOAD -------------------
void LoadScene() {
    entities.clear();

    std::string raw = readAllBytes(sceneSavePath);
    if (raw.empty()) return;

    size_t pos = 0;
    while (true) {
        pos = raw.find("\"asset\"", pos);
        if (pos == std::string::npos) break;

        size_t q1 = raw.find('"', raw.find(':', pos) + 1);
        size_t q2 = raw.find('"', q1 + 1);
        std::string asset = raw.substr(q1 + 1, q2 - q1 - 1);

        size_t xPos = raw.find("\"x\"", pos);
        size_t yPos = raw.find("\"y\"", pos);
        size_t wPos = raw.find("\"w\"", pos);
        size_t hPos = raw.find("\"h\"", pos);

        int x = atoi(raw.c_str() + raw.find(':', xPos) + 1);
        int y = atoi(raw.c_str() + raw.find(':', yPos) + 1);
        int w = atoi(raw.c_str() + raw.find(':', wPos) + 1);
        int h = atoi(raw.c_str() + raw.find(':', hPos) + 1);

        Entity e;
        e.assetFilename = std::wstring(asset.begin(), asset.end());
        e.id = e.assetFilename;
        e.x = x; e.y = y; e.w = w; e.h = h; e.layer = 1;

        entities.push_back(e);
        pos = hPos + 1;
    }
}

// ------------------------------------------------------------
// COORDINATE UTILITIES
// ------------------------------------------------------------
POINT ScreenToWorld(int sx, int sy) {
    int sceneLeft = 0;
    int sceneTop = toolbarH;
    float wx = (sx - sceneLeft - viewOffsetX) / viewZoom;
    float wy = (sy - sceneTop - viewOffsetY) / viewZoom;
    return POINT{ (int)wx, (int)wy };
}

POINT WorldToScreen(int wx, int wy) {
    int sceneLeft = 0;
    int sceneTop = toolbarH;
    int sx = (int)(wx * viewZoom + viewOffsetX) + sceneLeft;
    int sy = (int)(wy * viewZoom + viewOffsetY) + sceneTop;
    return POINT{ sx, sy };
}

// ------------------------------------------------------------
// WINDOW PROC
// ------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    switch (msg) {

    //---------------------------------------------------------
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        // Click in scene → place
        if (my > toolbarH && my < winH - assetsPanelH) {
            if (!assetFiles.empty()) {
                Bitmap* b = assetImages[selectedAssetIndex];
                int iw = b ? b->GetWidth() : 64;
                int ih = b ? b->GetHeight() : 64;

                POINT wp = ScreenToWorld(mx, my);

                Entity e;
                e.x = wp.x;
                e.y = wp.y;
                e.w = iw;
                e.h = ih;
                e.assetFilename = fs::path(assetFiles[selectedAssetIndex]).filename().wstring();
                e.id = e.assetFilename;

                entities.push_back(e);
                selectedEntity = (int)entities.size() - 1;
            }
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    //---------------------------------------------------------
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta > 0) viewZoom *= 1.1f;
        else viewZoom /= 1.1f;

        if (viewZoom < 0.1f) viewZoom = 0.1f;
        if (viewZoom > 4.0f) viewZoom = 4.0f;

        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    //---------------------------------------------------------
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);

        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.Clear(Color(40, 40, 45));

        // ------------------ SCENE BG -------------------
        SolidBrush sb(Color(60, 60, 70));
        g.FillRectangle(&sb, 0, toolbarH, winW - rightPanelW, winH - toolbarH - assetsPanelH);

        // ------------------ GRID ------------------------
        Pen pen(Color(70, 70, 80));

        for (int x = -2000; x < 4000; x += 40) {
            POINT p1 = WorldToScreen(x, -2000);
            POINT p2 = WorldToScreen(x, 4000);
            g.DrawLine(&pen, (INT)p1.x, (INT)p1.y, (INT)p2.x, (INT)p2.y);
        }

        for (int y = -2000; y < 4000; y += 40) {
            POINT p1 = WorldToScreen(-2000, y);
            POINT p2 = WorldToScreen(4000, y);
            g.DrawLine(&pen, (INT)p1.x, (INT)p1.y, (INT)p2.x, (INT)p2.y);
        }

        // ------------------ ENTITIES --------------------
        for (auto& e : entities) {
            POINT tl = WorldToScreen(e.x, e.y);

            Rect r(tl.x, tl.y, (INT)(e.w * viewZoom), (INT)(e.h * viewZoom));
            Bitmap* b = nullptr;

            for (size_t i = 0; i < assetFiles.size(); i++) {
                if (fs::path(assetFiles[i]).filename().wstring() == e.assetFilename)
                    b = assetImages[i];
            }

            if (b) g.DrawImage(b, r);
            else {
                SolidBrush red(Color(200, 80, 60));
                g.FillRectangle(&red, r);
            }
        }

        // ---------------- ASSET BAR --------------------
        SolidBrush ab(Color(22, 22, 22));
        g.FillRectangle(&ab, 0, winH - assetsPanelH, winW - rightPanelW, assetsPanelH);

        int x = 10;
        for (size_t i = 0; i < assetImages.size(); i++) {
            Bitmap* b = assetImages[i];
            if (b) {
                g.DrawImage(b, x, winH - assetsPanelH + 10, 64, 64);
            }
            x += 70;
        }

        // ---------------- RIGHT PANEL ------------------
        SolidBrush rp(Color(30, 30, 30));
        g.FillRectangle(&rp, winW - rightPanelW, 0, rightPanelW, winH);

        FontFamily ff(L"Segoe UI");
        Font font(&ff, 12);
        SolidBrush white(Color(235, 235, 235));

        g.DrawString(L"Inspector", -1, &font,
            PointF((REAL)(winW - rightPanelW + 10), 10), &white);

        if (selectedEntity >= 0 && selectedEntity < (int)entities.size()) {
            Entity& e = entities[selectedEntity];

            int baseY = 40;
            g.DrawString(e.assetFilename.c_str(), -1, &font,
                PointF((REAL)(winW - rightPanelW + 10), (REAL)baseY), &white);

            baseY += 20;

            std::wstring pos = L"Pos: " + std::to_wstring(e.x) + L"," + std::to_wstring(e.y);
            g.DrawString(pos.c_str(), -1, &font,
                PointF((REAL)(winW - rightPanelW + 10), (REAL)baseY), &white);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    //---------------------------------------------------------
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ------------------------------------------------------------
// WINMAIN
// ------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int nCmdShow) {
    GdiplusStartupInput gsi;
    GdiplusStartup(&gToken, &gsi, NULL);

    ScanAssets();
    LoadScene();

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = h;
    wc.lpszClassName = L"ThunderzEditor";
    RegisterClass(&wc);

    HWND wnd = CreateWindowEx(
        0, wc.lpszClassName,
        L"Thunderz Editor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winW, winH,
        NULL, NULL, h, NULL);

    ShowWindow(wnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gToken);
    return 0;
}
