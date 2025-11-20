// ThunderzEditor.cpp
// Thunderz Editor (single-file, improved)
// - Real sprite rendering (assets/*.bmp loaded as Gdiplus::Bitmap)
// - Transform move gizmo (select + drag to move)
// - Inspector edit controls (Win32 EDIT controls for X/Y/Z + Apply)
// - Double-buffered drawing (offscreen Gdiplus::Bitmap)
//
// Compile (MinGW / g++):
// g++ -std=c++17 -municode ThunderzEditor.cpp -o ThunderzEditor.exe -lgdiplus -lshlwapi -lgdi32
//
// Place resulting ThunderzEditor.exe in your E:\Thunderz_Game_Engine folder (replace the old editor exe).
//
// Notes:
// - Expects config.txt in editor folder with line: project_path=E:/Thunderz_Game_Engine/Projects/ExampleProject
// - Project should have assets/ and scenes/ subfolders.
// - This is a compact example; expand with undo/redo, parenting, thumbnails caching etc.

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM
#include <gdiplus.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cassert>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// ------------------------------
// Helper conversions
// ------------------------------
static std::wstring Utf8ToW(const std::string &s) {
    if (s.empty()) return {};
    int req = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out; out.resize(req);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), req);
    return out;
}
static std::string WToUtf8(const std::wstring &ws) {
    if (ws.empty()) return {};
    int req = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    std::string out; out.resize(req);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), req, NULL, NULL);
    return out;
}
static std::string WToA(const std::wstring &ws) {
    // CP_ACP narrow conversion (use for ifstream requiring char* path)
    if (ws.empty()) return {};
    int req = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    std::string out; out.resize(req);
    WideCharToMultiByte(CP_ACP, 0, ws.c_str(), (int)ws.size(), out.data(), req, NULL, NULL);
    return out;
}

// ------------------------------
// Basic editor data structures
// ------------------------------
struct Entity {
    std::string id;
    std::string asset; // filename in assets/
    int x = 0;
    int y = 0;
    int z = 0;
    int width = 64;
    int height = 64;
};

static std::vector<Entity> g_entities;
static std::map<std::string, Gdiplus::Bitmap*> g_bitmaps; // asset filename -> bitmap
static std::wstring g_projectPath = L"";
static HWND g_hWnd = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static int g_clientW = 1280, g_clientH = 720;
static int g_leftPanelW = 200;
static int g_rightPanelW = 240;

static int g_gridStep = 32;
static bool g_showGrid = true;

// selection/dragging
static int g_selectedIndex = -1;
static bool g_dragging = false;
static int g_dragStartX=0, g_dragStartY=0;
static int g_entityStartX=0, g_entityStartY=0;

// inspector controls
static HWND g_editX = NULL, g_editY = NULL, g_editZ = NULL, g_btnApply = NULL;

// double-buffer offscreen
static Bitmap* g_offscreenBmp = nullptr;

// ------------------------------
// Utilities: logging to editor.log (simple append)
// ------------------------------
static void AppendLog(const std::string &s) {
    std::wstring folder = g_projectPath.empty() ? fs::current_path().wstring() : g_projectPath;
    std::wstring logPath = folder + L"\\editor.log";
    std::ofstream f(WToA(logPath), std::ios::app);
    if (f) {
        f << s << "\n";
    }
}

// ------------------------------
// Read config.txt (project_path=...)
// ------------------------------
static bool ReadConfig() {
    // config located in exe folder
    wchar_t exePathW[MAX_PATH];
    GetModuleFileNameW(NULL, exePathW, MAX_PATH);
    fs::path exePath = exePathW;
    fs::path cfg = exePath.parent_path() / "config.txt";
    if (!fs::exists(cfg)) {
        AppendLog("config.txt not found");
        return false;
    }
    std::ifstream fi(WToA(cfg.wstring()));
    if (!fi) {
        AppendLog("Failed open config.txt");
        return false;
    }
    std::string line;
    while (std::getline(fi, line)) {
        if (line.rfind("project_path=", 0) == 0) {
            std::string p = line.substr(13);
            // convert to wstring
            g_projectPath = Utf8ToW(p);
            return true;
        }
    }
    AppendLog("project_path not found in config.txt");
    return false;
}

// ------------------------------
// Load assets bitmaps from project assets folder
// ------------------------------
static void ScanAssets() {
    g_bitmaps.clear();
    if (g_projectPath.empty()) return;
    fs::path assetsFolder = fs::path(g_projectPath) / "assets";
    if (!fs::exists(assetsFolder)) {
        AppendLog("assets folder not found: " + WToUtf8(assetsFolder.wstring()));
        return;
    }
    for (auto &p : fs::directory_iterator(assetsFolder)) {
        if (!p.is_regular_file()) continue;
        std::string ext = p.path().extension().string();
        for (auto &c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".bmp" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
            std::wstring pathW = p.path().wstring();
            // Load via Gdiplus::Bitmap - takes wide string (LPCWSTR)
            Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(pathW.c_str());
            if (bmp && bmp->GetLastStatus() == Ok) {
                g_bitmaps[p.path().filename().string()] = bmp;
                std::ostringstream msg;
                msg << "Found asset: " << p.path().filename().string();
                AppendLog(msg.str());
            } else {
                delete bmp;
                std::ostringstream msg;
                msg << "Asset load failed: " << p.path().filename().string();
                AppendLog(msg.str());
            }
        }
    }
}

// ------------------------------
// Scene Load/Save (very simple JSON-like text)
// ------------------------------
static bool LoadScene() {
    if (g_projectPath.empty()) return false;
    fs::path scenePath = fs::path(g_projectPath) / "scenes" / "main.scene";
    if (!fs::exists(scenePath)) {
        AppendLog("main.scene not found");
        return false;
    }
    std::ifstream fi(WToA(scenePath.wstring()));
    if (!fi) {
        AppendLog("Failed open main.scene");
        return false;
    }
    // very simple parser: read lines and extract entries of form:
    // { "id":"bg",   "asset":"bg.bmp",   "x":0,   "y":0,   "z":0 },
    g_entities.clear();
    std::string text;
    while (std::getline(fi, text)) {
        // crude parse: if line contains "id":
        size_t idpos = text.find("\"id\"");
        if (idpos != std::string::npos) {
            Entity e;
            // find id value
            size_t c1 = text.find('"', idpos + 4);
            size_t c2 = text.find('"', c1 + 1);
            c1 = text.find('"', c2 + 1);
            c2 = text.find('"', c1 + 1);
            if (c1!=std::string::npos && c2!=std::string::npos) {
                e.id = text.substr(c1+1, c2-c1-1);
            }
            // find asset
            size_t apos = text.find("\"asset\"");
            if (apos!=std::string::npos) {
                size_t s1 = text.find('"', apos+7);
                size_t s2 = text.find('"', s1+1);
                s1 = text.find('"', s2+1);
                s2 = text.find('"', s1+1);
                if (s1!=std::string::npos && s2!=std::string::npos) {
                    e.asset = text.substr(s1+1, s2-s1-1);
                }
            }
            // find x,y,z
            auto findInt = [&](const std::string &k)->int {
                size_t p = text.find(k);
                if (p==std::string::npos) return 0;
                size_t colon = text.find(':', p);
                if (colon==std::string::npos) return 0;
                size_t comma = text.find_first_of(",}", colon+1);
                std::string num = text.substr(colon+1, comma-colon-1);
                try { return stoi(num); } catch(...) { return 0; }
            };
            e.x = findInt("\"x\"");
            e.y = findInt("\"y\"");
            e.z = findInt("\"z\"");
            // size: if bitmap present use its size
            auto it = g_bitmaps.find(e.asset);
            if (it != g_bitmaps.end() && it->second) {
                e.width = it->second->GetWidth();
                e.height = it->second->GetHeight();
            } else {
                e.width = 64; e.height = 64;
            }
            g_entities.push_back(e);
        }
    }
    AppendLog("Loaded entities count: " + std::to_string(g_entities.size()));
    return true;
}

static bool SaveScene() {
    if (g_projectPath.empty()) return false;
    fs::path scenePath = fs::path(g_projectPath) / "scenes" / "main.scene";
    std::ofstream fo(WToA(scenePath.wstring()));
    if (!fo) {
        AppendLog("Save scene FAILED (open)");
        return false;
    }
    fo << "{\n  \"id\":\"main\",\n  \"entities\": [\n";
    for (size_t i=0;i<g_entities.size();++i) {
        Entity &e = g_entities[i];
        fo << "    { \"id\":\"" << e.id << "\", \"asset\":\"" << e.asset << "\", \"x\":" << e.x << ", \"y\":" << e.y << ", \"z\":" << e.z << " }";
        if (i+1 < g_entities.size()) fo << ",";
        fo << "\n";
    }
    fo << "  ],\n  \"script\": []\n}\n";
    AppendLog("Saved scene: OK");
    return true;
}

// ------------------------------
// Drawing helpers
// ------------------------------
static void DrawGrid(Graphics &g, int w, int h, int step) {
    Pen pen(Color(40,40,40), 1.0f);
    for (int x=0;x<=w; x+=step) {
        g.DrawLine(&pen, (REAL)x, 0.0f, (REAL)x, (REAL)h);
    }
    for (int y=0;y<=h; y+=step) {
        g.DrawLine(&pen, 0.0f, (REAL)y, (REAL)w, (REAL)y);
    }
}

static void DrawEntityToGraphics(Graphics &g, const Entity &e, bool highlight=false) {
    int x = e.x;
    int y = e.y;
    int w = e.width;
    int h = e.height;
    auto it = g_bitmaps.find(e.asset);
    if (it != g_bitmaps.end() && it->second) {
        Bitmap* bmp = it->second;
        // draw with scaling to e.width/e.height
        g.DrawImage(bmp, x, y, w, h);
    } else {
        SolidBrush brush(Color(160,80,80));
        Pen pen(Color(255,255,255), 2.0f);
        g.FillRectangle(&brush, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
        g.DrawRectangle(&pen, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
    }
    if (highlight) {
        Pen sel(Color(200,200,200), 3.0f);
        sel.SetDashStyle(DashStyleDash);
        g.DrawRectangle(&sel, (REAL)x-2, (REAL)y-2, (REAL)w+4, (REAL)h+4);
    }
}

// render entire editor contents to given Graphics (offscreen)
static void RenderEditorContents(Graphics &g, int w, int h) {
    // background
    SolidBrush bg(Color(30,30,30));
    g.FillRectangle(&bg, 0.0f, 0.0f, (REAL)w, (REAL)h);

    // left panel background
    SolidBrush lp(Color(25,25,25));
    g.FillRectangle(&lp, 0.0f, 0.0f, (REAL)g_leftPanelW, (REAL)h);

    // right panel background
    SolidBrush rp(Color(25,25,25));
    g.FillRectangle(&rp, (REAL)(w - g_rightPanelW), 0.0f, (REAL)g_rightPanelW, (REAL)h);

    // scene area background
    int sceneX = g_leftPanelW;
    int sceneW = w - g_leftPanelW - g_rightPanelW;
    SolidBrush sceneBg(Color(50,50,60));
    g.FillRectangle(&sceneBg, (REAL)sceneX, 0.0f, (REAL)sceneW, (REAL)h);

    // draw grid in scene area
    if (g_showGrid) {
        GraphicsState st = g.Save();
        g.TranslateTransform((REAL)sceneX, 0.0f);
        DrawGrid(g, sceneW, h, g_gridStep);
        g.Restore(st);
    }

    // draw scene contents (translate)
    GraphicsState sg = g.Save();
    g.TranslateTransform((REAL)sceneX, 0.0f);

    // draw entities by z-order (simple - ascending)
    std::vector<int> indices(g_entities.size());
    for (size_t i=0;i<indices.size();++i) indices[i] = (int)i;
    std::sort(indices.begin(), indices.end(), [](int a, int b){
        return g_entities[a].z < g_entities[b].z;
    });
    for (int idx : indices) {
        bool selected = (idx == g_selectedIndex);
        DrawEntityToGraphics(g, g_entities[idx], selected);
    }

    // draw transform gizmo for selected entity (simple center square)
    if (g_selectedIndex >= 0 && g_selectedIndex < (int)g_entities.size()) {
        Entity &E = g_entities[g_selectedIndex];
        int cx = E.x + E.width/2;
        int cy = E.y + E.height/2;
        SolidBrush gb(Color(120,120,120));
        Pen gp(Color(255,255,255), 2.0f);
        g.FillRectangle(&gb, (REAL)cx-8, (REAL)cy-8, 16.0f, 16.0f);
        g.DrawRectangle(&gp, (REAL)cx-8, (REAL)cy-8, 16.0f, 16.0f);
    }

    g.Restore(sg);

    // left panel: assets list drawn as thumbnails & filenames
    {
        int y = 10;
        int thumbX = 12;
        int thumbW = 64, thumbH = 64;
        for (auto &kv : g_bitmaps) {
            std::string name = kv.first;
            Bitmap* bmp = kv.second;
            // draw thumb background
            SolidBrush box(Color(40,40,40));
            g.FillRectangle(&box, (REAL)thumbX, (REAL)y, (REAL)thumbW, (REAL)thumbH);
            if (bmp) {
                // scale to fit 64x64
                float sw = (float)bmp->GetWidth() / (float)thumbW;
                float sh = (float)bmp->GetHeight() / (float)thumbH;
                float s = max(1.0f, max(sw, sh));
                int dw = (int)(bmp->GetWidth() / s);
                int dh = (int)(bmp->GetHeight() / s);
                int dx = thumbX + (thumbW - dw)/2;
                int dy = y + (thumbH - dh)/2;
                g.DrawImage(bmp, dx, dy, dw, dh);
            }
            // draw filename
            FontFamily ff(L"Segoe UI");
            Font f(&ff, 10, FontStyleRegular, UnitPixel);
            SolidBrush t(Color(200,200,200));
            std::wstring wname = Utf8ToW(name);
            g.DrawString(wname.c_str(), -1, &f, PointF((REAL)thumbX + thumbW + 8, (REAL)y + 10), &t);
            y += thumbH + 16;
        }
    }

    // right panel: inspector header and preview
    {
        int left = w - g_rightPanelW + 12;
        int y = 12;
        FontFamily ff(L"Segoe UI");
        Font fh(&ff, 14, FontStyleBold, UnitPixel);
        SolidBrush t(Color(220,220,220));
        g.DrawString(L"Inspector", -1, &fh, PointF((REAL)left, (REAL)y), &t);
        y += 36;
        if (g_selectedIndex >= 0 && g_selectedIndex < (int)g_entities.size()) {
            Entity &E = g_entities[g_selectedIndex];
            Font fsmall(&ff, 12, FontStyleRegular, UnitPixel);
            std::wstring idw = Utf8ToW(E.id);
            g.DrawString(idw.c_str(), -1, &fsmall, PointF((REAL)left, (REAL)y), &t);
            y += 24;
            // draw a small preview thumbnail
            auto it = g_bitmaps.find(E.asset);
            if (it != g_bitmaps.end() && it->second) {
                Bitmap* bmp = it->second;
                int pw = 96, ph = 96;
                g.DrawImage(bmp, left, y, pw, ph);
            } else {
                SolidBrush box(Color(80,80,80));
                g.FillRectangle(&box, (REAL)left, (REAL)y, 96.0f, 96.0f);
            }
        }
    }

    // bottom status
    {
        std::wstring s = L"Thunderz Editor - Advanced (stable build)";
        FontFamily ff(L"Segoe UI");
        Font fsmall(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush t(Color(200,200,200));
        g.DrawString(s.c_str(), -1, &fsmall, PointF(8.0f, (REAL)g_clientH-24), &t);
    }
}


// ------------------------------
// Window / input handling
// ------------------------------
static void CreateInspectorControls(HWND hwnd) {
    // Destroy if existing
    if (g_editX) DestroyWindow(g_editX), g_editX = NULL;
    if (g_editY) DestroyWindow(g_editY), g_editY = NULL;
    if (g_editZ) DestroyWindow(g_editZ), g_editZ = NULL;
    if (g_btnApply) DestroyWindow(g_btnApply), g_btnApply = NULL;

    int left = g_clientW - g_rightPanelW + 12;
    int y = 12 + 36 + 140; // below the inspector preview area
    int w = 60, h = 24;
    g_editX = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                              left, y, w, h, hwnd, (HMENU)1001, GetModuleHandle(NULL), NULL);
    g_editY = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                              left + 72, y, w, h, hwnd, (HMENU)1002, GetModuleHandle(NULL), NULL);
    g_editZ = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                              left + 144, y, 48, h, hwnd, (HMENU)1003, GetModuleHandle(NULL), NULL);

    g_btnApply = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 left + 200, y, 64, h, hwnd, (HMENU)2001, GetModuleHandle(NULL), NULL);
}

static void UpdateInspectorFields() {
    if (!g_editX || !g_editY || !g_editZ) return;
    if (g_selectedIndex >=0 && g_selectedIndex < (int)g_entities.size()) {
        Entity &E = g_entities[g_selectedIndex];
        std::wstring sx = Utf8ToW(std::to_string(E.x));
        std::wstring sy = Utf8ToW(std::to_string(E.y));
        std::wstring sz = Utf8ToW(std::to_string(E.z));
        SetWindowTextW(g_editX, sx.c_str());
        SetWindowTextW(g_editY, sy.c_str());
        SetWindowTextW(g_editZ, sz.c_str());
    } else {
        SetWindowTextW(g_editX, L"0");
        SetWindowTextW(g_editY, L"0");
        SetWindowTextW(g_editZ, L"0");
    }
}

static void ApplyInspectorFields() {
    if (g_selectedIndex >=0 && g_selectedIndex < (int)g_entities.size()) {
        wchar_t buf[64];
        GetWindowTextW(g_editX, buf, ARRAYSIZE(buf));
        int nx = _wtoi(buf);
        GetWindowTextW(g_editY, buf, ARRAYSIZE(buf));
        int ny = _wtoi(buf);
        GetWindowTextW(g_editZ, buf, ARRAYSIZE(buf));
        int nz = _wtoi(buf);
        // commit
        g_entities[g_selectedIndex].x = nx;
        g_entities[g_selectedIndex].y = ny;
        g_entities[g_selectedIndex].z = nz;
    }
}

// pick entity index by scene coordinates (client coords)
static int PickEntityAt(int sceneClientX, int sceneClientY) {
    int sx = sceneClientX - g_leftPanelW;
    int sy = sceneClientY;
    // iterate top-down (highest z first)
    std::vector<int> indices(g_entities.size());
    for (size_t i=0;i<indices.size();++i) indices[i] = (int)i;
    std::sort(indices.begin(), indices.end(), [](int a, int b){
        return g_entities[a].z > g_entities[b].z; // higher z first
    });
    for (int idx : indices) {
        Entity &E = g_entities[idx];
        if (sx >= E.x && sx <= E.x + E.width && sy >= E.y && sy <= E.y + E.height) {
            return idx;
        }
    }
    return -1;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        {
            // create inspector edit controls
            CreateInspectorControls(hwnd);
            UpdateInspectorFields();
        }
        return 0;
    case WM_SIZE:
        {
            g_clientW = LOWORD(lParam);
            g_clientH = HIWORD(lParam);
            if (g_offscreenBmp) { delete g_offscreenBmp; g_offscreenBmp = nullptr; }
            g_offscreenBmp = new Bitmap(max(1,g_clientW), max(1,g_clientH), PixelFormat32bppARGB);
            CreateInspectorControls(hwnd);
            UpdateInspectorFields();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            // pick on scene area?
            if (mx >= g_leftPanelW && mx < g_clientW - g_rightPanelW) {
                int idx = PickEntityAt(mx, my);
                g_selectedIndex = idx;
                UpdateInspectorFields();
                if (idx >= 0) {
                    // start drag
                    g_dragging = true;
                    g_dragStartX = mx;
                    g_dragStartY = my;
                    g_entityStartX = g_entities[idx].x;
                    g_entityStartY = g_entities[idx].y;
                    SetCapture(hwnd);
                }
            } else {
                // click outside scene deselect
                // do nothing
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        {
            if (g_dragging && g_selectedIndex >= 0 && g_selectedIndex < (int)g_entities.size()) {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                int dx = mx - g_dragStartX;
                int dy = my - g_dragStartY;
                int nx = g_entityStartX + dx;
                int ny = g_entityStartY + dy;
                // optional grid snap if SHIFT not pressed? (here always snap to grid)
                // bool snap = (GetKeyState(VK_CONTROL) & 0x8000) == 0;
                bool snap = true;
                if (snap) {
                    nx = (nx + g_gridStep/2) / g_gridStep * g_gridStep;
                    ny = (ny + g_gridStep/2) / g_gridStep * g_gridStep;
                }
                g_entities[g_selectedIndex].x = nx;
                g_entities[g_selectedIndex].y = ny;
                UpdateInspectorFields();
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        {
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                // finalize, maybe push undo stack later
                SaveScene(); // autosave on drop
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            if (id == 2001) { // Apply
                ApplyInspectorFields();
                SaveScene();
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // draw into offscreen Gdiplus::Bitmap then blit
            Graphics g(g_offscreenBmp);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            RenderEditorContents(g, g_clientW, g_clientH);

            // blit offscreen to window DC
            Graphics gdc(hdc);
            gdc.DrawImage(g_offscreenBmp, 0, 0, g_clientW, g_clientH);

            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ------------------------------
// Init / WinMain
// ------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // init GDI+
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL) != Ok) {
        MessageBoxW(NULL, L"Failed to init GDI+", L"Error", MB_OK|MB_ICONERROR);
        return -1;
    }

    // read config
    ReadConfig();

    // scan assets
    ScanAssets();

    // load scene
    LoadScene();

    // register window class
    const wchar_t CLASS_NAME[] = L"ThunderzEditorWndClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    // create window
    int ww = 1200, wh = 800;
    g_clientW = ww; g_clientH = wh;
    DWORD style = WS_OVERLAPPEDWINDOW;
    g_hWnd = CreateWindowExW(0, CLASS_NAME, L"Thunderz Editor - Advanced (stable build)", style,
                              CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        MessageBoxW(NULL, L"Failed to create window", L"Error", MB_OK|MB_ICONERROR);
        return -1;
    }

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    // create offscreen
    if (g_offscreenBmp) delete g_offscreenBmp;
    g_offscreenBmp = new Bitmap(max(1,g_clientW), max(1,g_clientH), PixelFormat32bppARGB);

    // main loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // cleanup
    if (g_offscreenBmp) delete g_offscreenBmp;
    for (auto &kv : g_bitmaps) {
        delete kv.second;
    }
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
