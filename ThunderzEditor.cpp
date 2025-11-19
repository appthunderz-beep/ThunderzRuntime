// ThunderzEditor.cpp - Phase1 Unity-style Editor (Compile with -std=gnu++17)
//
// Features:
// - Toolbar (Save, Load, AddAsset, Delete Selected)
// - Scene view with grid, pan (middle mouse) and zoom (mouse wheel)
// - Assets panel bottom with thumbnails (click to select)
// - Inspector on right showing selected entity (X,Y,W,H,Asset)
// - Click to place, drag-to-move, right-click to remove, Delete key to delete selected
// - Arrow keys nudge selected entity, S=save, L=load
//
// Notes: Editor expects assets under ..\Projects\ExampleProject\assets\ (relative to Tools\Editor.exe).
// Save writes to ..\Projects\ExampleProject\scenes\main.scene (UTF-8 JSON)

#define UNICODE
#define _UNICODE

#include <windows.h>
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

// Window layout sizes
int winW = 1100, winH = 700;
int rightPanelW = 300;
int assetsPanelH = 140;
int toolbarH = 40;
ULONG_PTR gToken;

struct Entity {
    std::wstring id;
    std::wstring assetFilename; // filename only
    int x, y;
    int w, h;
    int layer;
};

std::vector<Entity> entities;
std::vector<std::wstring> assetFiles; // full paths
std::vector<Bitmap*> assetImages;
int selectedAssetIndex = 0;
int selectedEntity = -1;

// View transform (for pan/zoom)
float viewZoom = 1.0f;
float viewOffsetX = 0.0f, viewOffsetY = 0.0f;
bool panning = false;
int panStartX = 0, panStartY = 0;
float panStartOffsetX = 0, panStartOffsetY = 0;

// Dragging entities
bool dragging = false;
int dragEntity = -1;
int dragOffsetX = 0, dragOffsetY = 0;

// Paths (relative to Tools\)
std::wstring projectAssetsDir = L"..\\Projects\\ExampleProject\\assets\\";
std::wstring sceneSavePath = L"..\\Projects\\ExampleProject\\scenes\\main.scene";

static std::string readAllBytes(const std::wstring &path) {
    std::ifstream ifs(std::string(path.begin(), path.end()), std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss; ss << ifs.rdbuf();
    return ss.str();
}

void ScanAssets() {
    for (auto b : assetImages) if (b) delete b;
    assetImages.clear();
    assetFiles.clear();

    try {
        if (fs::exists(projectAssetsDir) && fs::is_directory(projectAssetsDir)) {
            for (auto &p : fs::directory_iterator(projectAssetsDir)) {
                if (!p.is_regular_file()) continue;
                auto ext = p.path().extension().wstring();
                for (auto &c : ext) c = towlower(c);
                if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp") {
                    assetFiles.push_back(p.path().wstring());
                }
            }
        }
    } catch (...) { /* ignore */ }

    std::sort(assetFiles.begin(), assetFiles.end());
    for (auto &ap : assetFiles) {
        Bitmap* b = Bitmap::FromFile(ap.c_str());
        if (b && b->GetLastStatus() == Ok) assetImages.push_back(b);
        else assetImages.push_back(nullptr);
    }
    if (selectedAssetIndex >= (int)assetFiles.size()) selectedAssetIndex = (int)assetFiles.size() - 1;
    if (selectedAssetIndex < 0) selectedAssetIndex = 0;
}

// JSON save (simple)
void SaveScene() {
    std::wstringstream ss;
    ss << L"{\n  \"id\":\"main\",\n  \"entities\":[\n";
    for (size_t i = 0; i < entities.size(); ++i) {
        auto &e = entities[i];
        ss << L"    {\"id\":\"" << e.id << L"\",\"asset\":\"" << e.assetFilename << L"\",\"x\":" << e.x << L",\"y\":" << e.y << L",\"w\":"<<e.w<<L",\"h\":"<<e.h<<L",\"layer\":"<<e.layer<<L"}";
        if (i + 1 < entities.size()) ss << L",";
        ss << L"\n";
    }
    ss << L"  ]\n}\n";
    std::wstring ws = ss.str();
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, NULL, NULL);
    // write file
    std::ofstream ofs(std::string(sceneSavePath.begin(), sceneSavePath.end()), std::ios::binary);
    ofs << out;
    ofs.close();
}

// JSON load (very forgiving)
void LoadScene() {
    entities.clear();
    try {
        std::string raw = readAllBytes(sceneSavePath);
        if (raw.empty()) return;
        // naive parse: find blocks with "asset":"...", "x":num, "y":num, "w":num,"h":num
        size_t pos = 0;
        while (true) {
            pos = raw.find("{", pos);
            if (pos == std::string::npos) break;
            size_t end = raw.find("}", pos);
            if (end == std::string::npos) break;
            std::string block = raw.substr(pos, end - pos + 1);
            // asset
            size_t aset = block.find("\"asset\"");
            if (aset == std::string::npos) { pos = end + 1; continue; }
            size_t c = block.find(':', aset);
            size_t q1 = block.find('"', c);
            size_t q2 = block.find('"', q1 + 1);
            std::string asset = (q1 != std::string::npos && q2 != std::string::npos) ? block.substr(q1 + 1, q2 - q1 - 1) : "";
            auto str_to_w = [&](const std::string &s) {
                return std::wstring(s.begin(), s.end());
            };
            int x = 0, y = 0, w = 64, h = 64;
            size_t xp = block.find("\"x\"");
            if (xp != std::string::npos) {
                size_t cc = block.find(':', xp);
                x = atoi(block.c_str() + cc + 1);
            }
            size_t yp = block.find("\"y\"");
            if (yp != std::string::npos) {
                size_t cc = block.find(':', yp);
                y = atoi(block.c_str() + cc + 1);
            }
            size_t wp = block.find("\"w\"");
            if (wp != std::string::npos) {
                size_t cc = block.find(':', wp);
                w = atoi(block.c_str() + cc + 1);
            }
            size_t hp = block.find("\"h\"");
            if (hp != std::string::npos) {
                size_t cc = block.find(':', hp);
                h = atoi(block.c_str() + cc + 1);
            }
            Entity e;
            e.assetFilename = str_to_w(asset);
            e.id = e.assetFilename + std::to_wstring(entities.size() + 1);
            e.x = x; e.y = y; e.w = w; e.h = h; e.layer = 1;
            entities.push_back(e);
            pos = end + 1;
        }
    } catch (...) {}
}

// Helpers: convert screen-to-world and world-to-screen
POINT ScreenToWorld(int sx, int sy) {
    // subtract panels and toolbar
    int sceneLeft = 0;
    int sceneTop = toolbarH;
    float wx = (sx - sceneLeft - viewOffsetX) / viewZoom;
    float wy = (sy - sceneTop - viewOffsetY) / viewZoom;
    POINT p{ (int)wx, (int)wy };
    return p;
}
POINT WorldToScreen(int wxi, int wyi) {
    int sceneLeft = 0;
    int sceneTop = toolbarH;
    int sx = (int)(wxi * viewZoom + viewOffsetX) + sceneLeft;
    int sy = (int)(wyi * viewZoom + viewOffsetY) + sceneTop;
    return POINT{ sx, sy };
}

// UI helpers: draw button (returns true if clicked)
bool DrawButton(Graphics &g, const Rect &r, const WCHAR* text, bool hovered, bool pressed) {
    SolidBrush bg(pressed ? Color(230, 60, 60, 60) : hovered ? Color(200, 40, 40, 40) : Color(160, 30, 30, 30));
    g.FillRectangle(&bg, r);
    SolidBrush brush(Color(255, 255, 255, 255));
    FontFamily ff(L"Segoe UI"); Font f(&ff, 12, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    RectF rf((REAL)r.X, (REAL)r.Y, (REAL)r.Width, (REAL)r.Height);
    g.DrawString(text, -1, &f, rf, &sf, &brush);
    return false; // actual click handling done separately
}

// Hit test for toolbar and UI done in WndProc

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool leftDown = false;
    static bool toolbarHoverSave = false, toolbarHoverLoad = false, toolbarHoverAdd = false, toolbarHoverDelete = false;
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        leftDown = true;

        // Toolbar click area
        int tbRight = 300;
        RECT saveR = { 8, 6, 78, 34 };
        RECT loadR = { 86, 6, 156, 34 };
        RECT addR = { 164, 6, 234, 34 };
        RECT delR = { 242, 6, 312, 34 };

        // check toolbar buttons
        if (mx >= saveR.left && mx <= saveR.right && my >= saveR.top && my <= saveR.bottom) {
            SaveScene();
            return 0;
        }
        if (mx >= loadR.left && mx <= loadR.right && my >= loadR.top && my <= loadR.bottom) {
            LoadScene();
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        if (mx >= addR.left && mx <= addR.right && my >= addR.top && my <= addR.bottom) {
            // Add asset: toggle via asset selection (no file dialog for now) - just cycle
            if (!assetFiles.empty()) selectedAssetIndex = (selectedAssetIndex + 1) % assetFiles.size();
            return 0;
        }
        if (mx >= delR.left && mx <= delR.right && my >= delR.top && my <= delR.bottom) {
            if (selectedEntity >= 0 && selectedEntity < (int)entities.size()) {
                entities.erase(entities.begin() + selectedEntity);
                selectedEntity = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        // Assets panel click (bottom)
        int assetsTop = winH - assetsPanelH;
        if (my >= assetsTop) {
            int idx = (mx / 80); // simple grid columns
            int colWidth = 80;
            // compute which thumbnail was clicked
            int perRow = (winW - rightPanelW) / colWidth;
            if (perRow < 1) perRow = 1;
            int tx = mx / colWidth;
            int ty = (my - assetsTop) / 80;
            int which = ty * perRow + tx;
            if (which >= 0 && which < (int)assetFiles.size()) {
                selectedAssetIndex = which;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }

        // Scene view click -> place or select or drag
        if (my > toolbarH && my < winH - assetsPanelH) {
            POINT wp = ScreenToWorld(mx, my);
            // check existing entity hit (topmost first)
            for (int i = (int)entities.size() - 1; i >= 0; --i) {
                Entity &e = entities[i];
                if (wp.x >= e.x && wp.x <= e.x + e.w && wp.y >= e.y && wp.y <= e.y + e.h) {
                    selectedEntity = i;
                    dragging = true;
                    dragEntity = i;
                    dragOffsetX = wp.x - e.x;
                    dragOffsetY = wp.y - e.y;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }
            // not clicked on an entity -> place new entity from selected asset
            if (!assetFiles.empty() && selectedAssetIndex >= 0 && selectedAssetIndex < (int)assetFiles.size()) {
                Entity ne;
                std::wstring fn = fs::path(assetFiles[selectedAssetIndex]).filename().wstring();
                ne.assetFilename = fn;
                ne.id = fn + std::to_wstring(entities.size() + 1);
                ne.x = wp.x;
                ne.y = wp.y;
                Bitmap* b = assetImages[selectedAssetIndex];
                if (b) { ne.w = b->GetWidth(); ne.h = b->GetHeight(); }
                else { ne.w = 64; ne.h = 64; }
                ne.layer = 1;
                entities.push_back(ne);
                selectedEntity = (int)entities.size() - 1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }

        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        if (dragging && dragEntity >= 0 && dragEntity < (int)entities.size()) {
            POINT wp = ScreenToWorld(mx, my);
            entities[dragEntity].x = wp.x - dragOffsetX;
            entities[dragEntity].y = wp.y - dragOffsetY;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        if (panning) {
            int dx = mx - panStartX, dy = my - panStartY;
            viewOffsetX = panStartOffsetX + dx;
            viewOffsetY = panStartOffsetY + dy;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        dragging = false;
        dragEntity = -1;
        return 0;

    case WM_RBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        POINT wp = ScreenToWorld(mx, my);
        // remove topmost entity at spot
        for (int i = (int)entities.size() - 1; i >= 0; --i) {
            Entity &e = entities[i];
            if (wp.x >= e.x && wp.x <= e.x + e.w && wp.y >= e.y && wp.y <= e.y + e.h) {
                entities.erase(entities.begin() + i);
                if (selectedEntity == i) selectedEntity = -1;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float oldZoom = viewZoom;
        if (delta > 0) viewZoom *= 1.1f;
        else viewZoom /= 1.1f;
        if (viewZoom < 0.1f) viewZoom = 0.1f;
        if (viewZoom > 4.0f) viewZoom = 4.0f;
        // keep viewOffset stable under zoom (not perfect but ok)
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_MBUTTONDOWN: {
        // start panning
        panning = true;
        panStartX = GET_X_LPARAM(lParam);
        panStartY = GET_Y_LPARAM(lParam);
        panStartOffsetX = viewOffsetX;
        panStartOffsetY = viewOffsetY;
        return 0;
    }
    case WM_MBUTTONUP:
        panning = false;
        return 0;

    case WM_KEYDOWN: {
        if (wParam == 'S') { SaveScene(); return 0; }
        if (wParam == 'L') { LoadScene(); InvalidateRect(hWnd, NULL, FALSE); return 0; }
        if (wParam == VK_DELETE) {
            if (selectedEntity >= 0 && selectedEntity < (int)entities.size()) {
                entities.erase(entities.begin() + selectedEntity);
                selectedEntity = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }
        // nudge selected entity
        if (selectedEntity >= 0 && selectedEntity < (int)entities.size()) {
            int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            if (wParam == VK_LEFT) { entities[selectedEntity].x -= step; InvalidateRect(hWnd, NULL, FALSE); }
            if (wParam == VK_RIGHT) { entities[selectedEntity].x += step; InvalidateRect(hWnd, NULL, FALSE); }
            if (wParam == VK_UP) { entities[selectedEntity].y -= step; InvalidateRect(hWnd, NULL, FALSE); }
            if (wParam == VK_DOWN) { entities[selectedEntity].y += step; InvalidateRect(hWnd, NULL, FALSE); }
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Color(40, 40, 45));

        // TOOLBAR
        SolidBrush tb(Color(200, 30, 30, 30));
        g.FillRectangle(&tb, 0, 0, winW - rightPanelW, toolbarH);
        // draw buttons (simple rectangles)
        SolidBrush btnBg(Color(200, 60, 60, 60));
        FontFamily ff(L"Segoe UI"); Font f(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush white(Color(255, 255, 255, 255));
        Rect saveR(8, 6, 70, 28); g.FillRectangle(&btnBg, saveR); g.DrawString(L"Save (S)", -1, &f, RectF((REAL)saveR.X, (REAL)saveR.Y, (REAL)saveR.Width, (REAL)saveR.Height), &white);
        Rect loadR(86, 6, 70, 28); g.FillRectangle(&btnBg, loadR); g.DrawString(L"Load (L)", -1, &f, RectF((REAL)loadR.X, (REAL)loadR.Y, (REAL)loadR.Width, (REAL)loadR.Height), &white);
        Rect addR(164,6,70,28); g.FillRectangle(&btnBg, addR); g.DrawString(L"Cycle Asset", -1, &f, RectF((REAL)addR.X, (REAL)addR.Y, (REAL)addR.Width, (REAL)addR.Height), &white);
        Rect delR(242,6,70,28); g.FillRectangle(&btnBg, delR); g.DrawString(L"Delete", -1, &f, RectF((REAL)delR.X, (REAL)delR.Y, (REAL)delR.Width, (REAL)delR.Height), &white);

        // Scene view area
        int sceneLeft = 0;
        int sceneTop = toolbarH;
        int sceneW = winW - rightPanelW;
        int sceneH = winH - toolbarH - assetsPanelH;
        // background for scene
        SolidBrush sceneBg(Color(55, 60, 70));
        g.FillRectangle(&sceneBg, sceneLeft, sceneTop, sceneW, sceneH);

        // draw grid (transform with zoom/offset)
        int gridSize = (int)(40 * viewZoom);
        if (gridSize < 8) gridSize = 8;
        Pen gridPen(Color(80, 80, 90));
        for (int gx = -1000; gx <= 5000; gx += gridSize) {
            // world x to screen
            POINT p1 = WorldToScreen(gx, -10000);
            POINT p2 = WorldToScreen(gx, 10000);
            g.DrawLine(&gridPen, p1.x, p1.y, p2.x, p2.y);
        }
        for (int gy = -1000; gy <= 5000; gy += gridSize) {
            POINT p1 = WorldToScreen(-10000, gy);
            POINT p2 = WorldToScreen(10000, gy);
            g.DrawLine(&gridPen, p1.x, p1.y, p2.x, p2.y);
        }

        // draw entities
        for (size_t i = 0; i < entities.size(); ++i) {
            Entity &e = entities[i];
            POINT tl = WorldToScreen(e.x, e.y);
            int sw = (int)(e.w * viewZoom), sh = (int)(e.h * viewZoom);
            Rect dst(tl.x, tl.y, sw, sh);
            // find asset image
            for (size_t ai = 0; ai < assetFiles.size(); ++ai) {
                if (fs::path(assetFiles[ai]).filename().wstring() == e.assetFilename) {
                    if (assetImages[ai]) {
                        g.DrawImage(assetImages[ai], dst);
                    } else {
                        SolidBrush bb(Color(220, 80, 80));
                        g.FillRectangle(&bb, dst);
                    }
                    break;
                }
            }
            // highlight if selected
            if ((int)i == selectedEntity) {
                Pen sel(Color(255, 255, 200, 40), 3);
                g.DrawRectangle(&sel, dst);
            }
        }

        // RIGHT PANEL (inspector)
        Rect rp(sceneLeft + sceneW, 0, rightPanelW, winH);
        SolidBrush rpBg(Color(220, 20, 20, 20));
        g.FillRectangle(&rpBg, rp);
        // Inspector header
        Font hfont(&ff, 14, FontStyleBold, UnitPixel);
        g.DrawString(L"Inspector", -1, &hfont, RectF(sceneLeft + sceneW + 8, 8, rightPanelW - 16, 24), &white);
        // show selected entity properties
        Font nf(&ff, 12, FontStyleRegular, UnitPixel);
        if (selectedEntity >= 0 && selectedEntity < (int)entities.size()) {
            Entity &sel = entities[selectedEntity];
            std::wstring lines[] = {
                L"ID: " + sel.id,
                L"Asset: " + sel.assetFilename,
                L"X: " + std::to_wstring(sel.x),
                L"Y: " + std::to_wstring(sel.y),
                L"W: " + std::to_wstring(sel.w),
                L"H: " + std::to_wstring(sel.h)
            };
            for (int i = 0; i < 6; ++i) {
                g.DrawString(lines[i].c_str(), -1, &nf, RectF(sceneLeft + sceneW + 8, 40 + i * 20, rightPanelW - 16, 20), &white);
            }
        } else {
            g.DrawString(L"No selection", -1, &nf, RectF(sceneLeft + sceneW + 8, 40, rightPanelW - 16, 20), &white);
        }

        // ASSETS PANEL (bottom)
        Rect assetsRect(0, winH - assetsPanelH, winW - rightPanelW, assetsPanelH);
        SolidBrush assetsBg(Color(200, 18, 18, 18));
        g.FillRectangle(&assetsBg, assetsRect);
        // draw thumbnails across the bottom-left area
        int thumbX = 4, thumbY = winH - assetsPanelH + 8;
        int thumbW = 72, thumbH = 72;
        int thumbsPerRow = (sceneW - 16) / (thumbW + 8);
        if (thumbsPerRow <= 0) thumbsPerRow = 1;
        for (size_t i = 0; i < assetImages.size(); ++i) {
            int ix = (int)i % thumbsPerRow;
            int iy = (int)i / thumbsPerRow;
            int dx = thumbX + ix * (thumbW + 8);
            int dy = thumbY + iy * (thumbH + 8);
            Rect tr(dx, dy, thumbW, thumbH);
            SolidBrush tbg((int)i == selectedAssetIndex ? Color(255, 120, 120, 60) : Color(255, 40, 40, 40));
            g.FillRectangle(&tbg, tr);
            Bitmap *b = assetImages[i];
            if (b) {
                int iw = b->GetWidth(), ih = b->GetHeight();
                float sx = (float)tr.Width / (float)iw;
                float sy = (float)tr.Height / (float)ih;
                float sc = (sx < sy) ? sx : sy;
                if (sc <= 0) sc = 1.0f;
                int dw = (int)(iw * sc), dh = (int)(ih * sc);
                int ddx = tr.X + (tr.Width - dw) / 2;
                int ddy = tr.Y + (tr.Height - dh) / 2;
                g.DrawImage(b, ddx, ddy, dw, dh);
            }
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&gToken, &gsi, NULL) != Ok) return -1;

    ScanAssets();
    // Try loading existing scene on start
    LoadScene();

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ThunderzEditor";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Editor - Thunderz Engine (Phase1)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    for (auto b : assetImages) if (b) delete b;
    GdiplusShutdown(gToken);
    return 0;
}
