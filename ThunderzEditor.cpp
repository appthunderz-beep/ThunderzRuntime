// ThunderzEditor_debug_patched.cpp - Debug build (GDI+ startup moved before bitmap loads)
// Replace your ThunderzEditor.cpp with this file. It will create `editor.log` next to the EXE
// and print exactly what it reads: config, project path, assets found (with sizes), main.scene contents

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <iomanip>

using namespace Gdiplus;
namespace fs = std::filesystem;
#pragma comment(lib, "gdiplus.lib")

// Window layout
int winW = 1100, winH = 700;
int rightPanelW = 300;
int assetsPanelH = 140;
int toolbarH = 40;
ULONG_PTR gToken;

// Logging
static std::ofstream gLog;
void Log(const std::string &s) {
    if (!gLog.is_open()) return;
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    gLog << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S") << " - " << s << std::endl;
}

// Utilities for wide/narrow conversions
static std::string ws_to_s(const std::wstring &ws) {
    if (ws.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    std::string s(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), s.data(), size_needed, NULL, NULL);
    return s;
}
static std::wstring s_to_ws(const std::string &s) {
    if (s.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring ws(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), size_needed);
    return ws;
}

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

float viewZoom = 1.0f;
float viewOffsetX = 0.0f, viewOffsetY = 0.0f;

// Paths & config
std::wstring exeFolder; // absolute folder of EXE
std::wstring configPath; // absolute config.txt path
std::wstring projectPath; // absolute project path
std::wstring assetsDir;   // absolute assets dir
std::wstring scenesDir;   // absolute scenes dir
std::wstring mainScenePath; // absolute main.scene

// default relative project
std::wstring defaultRelProject = L"Projects\\ExampleProject";

// --------------------- FILE HELPERS ---------------------
static std::string readAllBytesNarrow(const std::wstring& path) {
    std::ifstream ifs(ws_to_s(path), std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss; ss << ifs.rdbuf();
    return ss.str();
}

// ------------------- LOGGED ASSET SCANNER -------------------
void ScanAssets() {
    Log("Scanning assets in: " + ws_to_s(assetsDir));

    for (auto b : assetImages) if (b) delete b;
    assetImages.clear();
    assetFiles.clear();

    try {
        if (fs::exists(assetsDir) && fs::is_directory(assetsDir)) {
            for (auto& p : fs::directory_iterator(assetsDir)) {
                if (!p.is_regular_file()) continue;
                auto ext = p.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
                if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp")
                    assetFiles.push_back(p.path().wstring());
            }
        } else {
            Log("Assets dir missing or not a directory: " + ws_to_s(assetsDir));
        }
    } catch (std::exception &e) {
        Log(std::string("Exception scanning assets: ") + e.what());
    }

    std::sort(assetFiles.begin(), assetFiles.end());

    for (auto& path : assetFiles) {
        Bitmap* b = Bitmap::FromFile(path.c_str());
        if (b && b->GetLastStatus() == Ok) {
            assetImages.push_back(b);
            try {
                auto fsize = fs::file_size(path);
                Log("Found asset: " + ws_to_s(fs::path(path).filename().wstring()) + " (" + std::to_string((long long)fsize) + " bytes)");
            } catch (...) {}
        } else {
            assetImages.push_back(nullptr);
            Log("Asset failed to load (bitmap null or bad status): " + ws_to_s(path));
        }
    }

    if (selectedAssetIndex >= (int)assetFiles.size())
        selectedAssetIndex = std::max(0, (int)assetFiles.size() - 1);
}

// ------------------- SCENE SAVE/LOAD (with logging) -------------------
void SaveScene() {
    Log("Saving scene to: " + ws_to_s(mainScenePath));
    std::wstringstream ss;
    ss << L"{\n  \"id\": \"main\",\n  \"entities\": [\n";
    for (size_t i = 0; i < entities.size(); i++) {
        Entity& e = entities[i];
        ss << L"    {\"id\":\"" << e.id << L"\",\"asset\":\"" << e.assetFilename << L"\",\"x\":" << e.x
           << L",\"y\":" << e.y << L",\"w\":" << e.w << L",\"h\":" << e.h << L"}";
        if (i + 1 < entities.size()) ss << L",";
        ss << L"\n";
    }
    ss << L"  ],\n  \"script\": []\n}\n";
    std::wstring ws = ss.str();
    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, utf8.data(), size, NULL, NULL);
    std::ofstream ofs(ws_to_s(mainScenePath), std::ios::binary);
    ofs << utf8;
    Log("Scene saved (bytes written: " + std::to_string((long long)utf8.size()) + ")");
}

void LoadScene() {
    Log("Loading scene from: " + ws_to_s(mainScenePath));
    entities.clear();
    std::string raw = readAllBytesNarrow(mainScenePath);
    if (raw.empty()) {
        Log("Scene file empty or missing: " + ws_to_s(mainScenePath));
        return;
    }
    Log("Scene file content (first 1024 bytes):\n" + raw.substr(0, 1024));
    // Very simple parser: find occurrences of "asset" and read numeric x,y,w,h
    size_t pos = 0;
    while (true) {
        pos = raw.find("\"asset\"", pos);
        if (pos == std::string::npos) break;
        size_t q1 = raw.find('\"', raw.find(':', pos) + 1);
        size_t q2 = raw.find('\"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string asset = raw.substr(q1 + 1, q2 - q1 - 1);
        size_t xPos = raw.find("\"x\"", pos);
        size_t yPos = raw.find("\"y\"", pos);
        size_t wPos = raw.find("\"w\"", pos);
        size_t hPos = raw.find("\"h\"", pos);
        int x = 0, y = 0, w = 64, h = 64;
        if (xPos != std::string::npos) x = atoi(raw.c_str() + raw.find(':', xPos) + 1);
        if (yPos != std::string::npos) y = atoi(raw.c_str() + raw.find(':', yPos) + 1);
        if (wPos != std::string::npos) w = atoi(raw.c_str() + raw.find(':', wPos) + 1);
        if (hPos != std::string::npos) h = atoi(raw.c_str() + raw.find(':', hPos) + 1);
        Entity e;
        e.assetFilename = s_to_ws(asset);
        e.id = e.assetFilename;
        e.x = x; e.y = y; e.w = w; e.h = h; e.layer = 1;
        entities.push_back(e);
        pos = (hPos != std::string::npos) ? hPos + 1 : pos + 1;
    }
    Log("Loaded entities count: " + std::to_string((long long)entities.size()));
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
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int assetsTop = winH - assetsPanelH;
        if (my >= assetsTop) {
            int thumbW = 70;
            int perRow = std::max(1, (winW - rightPanelW) / thumbW);
            int tx = mx / thumbW;
            int ty = (my - assetsTop) / 80;
            int which = ty * perRow + tx;
            if (which >= 0 && which < (int)assetFiles.size()) {
                selectedAssetIndex = which;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }
        if (my > toolbarH && my < winH - assetsPanelH) {
            POINT wp = ScreenToWorld(mx, my);
            for (int i = (int)entities.size() - 1; i >= 0; --i) {
                Entity &e = entities[i];
                if (wp.x >= e.x && wp.x <= e.x + e.w && wp.y >= e.y && wp.y <= e.y + e.h) {
                    selectedEntity = i;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }
            if (!assetFiles.empty()) {
                Entity e;
                e.x = wp.x; e.y = wp.y;
                Bitmap* b = assetImages[selectedAssetIndex];
                e.w = b ? b->GetWidth() : 64;
                e.h = b ? b->GetHeight() : 64;
                e.assetFilename = fs::path(assetFiles[selectedAssetIndex]).filename().wstring();
                e.id = e.assetFilename + std::to_wstring(entities.size() + 1);
                entities.push_back(e);
                selectedEntity = (int)entities.size() - 1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta > 0) viewZoom *= 1.1f; else viewZoom /= 1.1f;
        if (viewZoom < 0.1f) viewZoom = 0.1f;
        if (viewZoom > 4.0f) viewZoom = 4.0f;
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.Clear(Color(40, 40, 45));

        SolidBrush sb(Color(60, 60, 70));
        g.FillRectangle(&sb, 0, toolbarH, winW - rightPanelW, winH - toolbarH - assetsPanelH);

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

        for (auto& e : entities) {
            POINT tl = WorldToScreen(e.x, e.y);
            Rect r(tl.x, tl.y, (INT)(e.w * viewZoom), (INT)(e.h * viewZoom));
            Bitmap* b = nullptr;
            for (size_t i = 0; i < assetFiles.size(); i++) {
                if (fs::path(assetFiles[i]).filename().wstring() == e.assetFilename)
                    b = assetImages[i];
            }
            if (b) g.DrawImage(b, r);
            else { SolidBrush red(Color(200, 80, 60)); g.FillRectangle(&red, r); }
        }

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

        SolidBrush rp(Color(30, 30, 30));
        g.FillRectangle(&rp, winW - rightPanelW, 0, rightPanelW, winH);

        FontFamily ff(L"Segoe UI");
        Font font(&ff, 12);
        SolidBrush white(Color(235, 235, 235));

        g.DrawString(L"Inspector", -1, &font, PointF((REAL)(winW - rightPanelW + 10), 10), &white);

        if (selectedEntity >= 0 && selectedEntity < (int)entities.size()) {
            Entity& e = entities[selectedEntity];
            int baseY = 40;
            g.DrawString(e.assetFilename.c_str(), -1, &font, PointF((REAL)(winW - rightPanelW + 10), (REAL)baseY), &white);
            baseY += 20;
            std::wstring pos = L"Pos: " + std::to_wstring(e.x) + L"," + std::to_wstring(e.y);
            g.DrawString(pos.c_str(), -1, &font, PointF((REAL)(winW - rightPanelW + 10), (REAL)baseY), &white);
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

// ------------------------------------------------------------
// WINMAIN
// ------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // open log
    gLog.open("editor.log", std::ios::out | std::ios::trunc);
    if (!gLog.is_open()) return -1;
    Log("--- Thunderz Editor (debug) startup ---");

    // find exe folder
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    fs::path p(path);
    exeFolder = p.parent_path().wstring();
    Log("EXE folder: " + ws_to_s(exeFolder));

    // config path
    configPath = exeFolder + L"\\config.txt";
    Log("Config path: " + ws_to_s(configPath));

    // read config.txt
    std::wstring cfgLine;
    if (fs::exists(configPath)) {
        std::ifstream cfg(ws_to_s(configPath));
        std::string tmp;
        std::getline(cfg, tmp);
        cfgLine = s_to_ws(tmp);
        Log("Read config line: " + tmp);
    } else {
        Log("config.txt not found at expected location. Will try default project path.");
    }

    // parse config line
    if (!cfgLine.empty()) {
        // accept forms: project=..., project_path=..., path=...
        std::string narrow = ws_to_s(cfgLine);
        std::transform(narrow.begin(), narrow.end(), narrow.begin(), ::tolower);
        size_t pos = narrow.find("project_path=");
        if (pos != std::string::npos) {
            // extract from wide string using the same index
            std::wstring val_w = cfgLine.substr((int)pos + 13); // after project_path=
            // trim CR/LF from wide string
            while (!val_w.empty() && (val_w.back() == L'\r' || val_w.back() == L'\n')) val_w.pop_back();
            std::string val = ws_to_s(val_w);
            // detect absolute path by checking for drive letter or starting slash
            bool isAbs = (val.find(':') != std::string::npos) || (!val.empty() && (val[0] == '\\' || val[0] == '/'));
            if (isAbs) projectPath = val_w;
            else projectPath = exeFolder + L"\\" + val_w;
        } else {
            // try simpler key
            pos = narrow.find("project=");
            if (pos != std::string::npos) {
                std::wstring val_w = cfgLine.substr((int)pos + 8);
                while (!val_w.empty() && (val_w.back() == L'\r' || val_w.back() == L'\n')) val_w.pop_back();
                std::string val = ws_to_s(val_w);
                bool isAbs = (val.find(':') != std::string::npos) || (!val.empty() && (val[0] == '\\' || val[0] == '/'));
                if (isAbs) projectPath = val_w;
                else projectPath = exeFolder + L"\\" + val_w;
            }
        }
    }

    // if still empty, fallback to default relative project
    if (projectPath.empty()) {
        projectPath = exeFolder + L"\\" + defaultRelProject;
        Log("Using fallback project path: " + ws_to_s(projectPath));
    } else {
        Log("Resolved project path: " + ws_to_s(projectPath));
    }

    // set assets/scenes paths
    assetsDir = projectPath + L"\\assets";
    scenesDir = projectPath + L"\\scenes";
    mainScenePath = scenesDir + L"\\main.scene";

    // Create missing dirs automatically (debug mode)
    try {
        if (!fs::exists(projectPath)) { fs::create_directories(projectPath); Log("Created project folder: " + ws_to_s(projectPath)); }
        if (!fs::exists(assetsDir)) { fs::create_directories(assetsDir); Log("Created assets folder: " + ws_to_s(assetsDir)); }
        if (!fs::exists(scenesDir)) { fs::create_directories(scenesDir); Log("Created scenes folder: " + ws_to_s(scenesDir)); }
    } catch (std::exception &e) {
        Log(std::string("Exception creating folders: ") + e.what());
    }

    // list files in project
    try {
        Log("Project contents:");
        for (auto &it : fs::directory_iterator(projectPath)) {
            Log(" - " + ws_to_s(it.path().filename().wstring()));
        }
    } catch (...) {}

    // -------------------------
    // IMPORTANT CHANGE: Initialize GDI+ BEFORE loading bitmaps
    // -------------------------
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&gToken, &gsi, NULL) != Ok) {
        Log("GdiplusStartup failed");
        return -1;
    }
    Log("Gdiplus initialized successfully");

    // scan assets and load scene (now GDI+ is ready so Bitmap::FromFile will work)
    ScanAssets();
    LoadScene();

    // register window
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ThunderzEditor";
    RegisterClass(&wc);

    HWND wnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Editor (debug)", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, NULL, NULL, hInstance, NULL);
    ShowWindow(wnd, nCmdShow);

    MSG msg;
    Log("Entering main loop");
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Log("Shutting down, cleaning up");
    for (auto b : assetImages) if (b) delete b;
    GdiplusShutdown(gToken);
    gLog.close();
    return 0;
}

