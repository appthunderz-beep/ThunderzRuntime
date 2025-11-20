// ThunderzEditor.cpp
// Advanced-ish editor but kept stable and clean for MinGW/GCC + GDI+
// Build with: g++ -std=c++17 -municode ThunderzEditor.cpp -o ThunderzEditor.exe -lgdiplus -lshlwapi -lgdi32

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM
#include <gdiplus.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <mutex>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

// ------------------------------- Config / paths -------------------------------
static std::string g_exeFolder;
static std::string g_projectPath; // e.g. E:/Thunderz_Game_Engine/Projects/ExampleProject
static std::string g_assetsPath;
static std::string g_scenesPath;
static std::string g_logPath;
static std::mutex g_logMutex;

void AppendLog(const std::string &t) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::ofstream f(g_logPath, std::ios::app);
    if (f) {
        f << t << std::endl;
        f.close();
    }
}

// ------------------------------- Simple data models -------------------------------
struct Entity {
    std::string id;
    std::string asset;
    int x = 0;
    int y = 0;
    int z = 0;
    Bitmap *bmp = nullptr; // pointer, not owned by entity (assets map owns)
};

struct AssetItem {
    std::string filename;
    Bitmap *bmp = nullptr;
    int w = 0, h = 0;
};

static std::vector<Entity> g_entities;
static std::vector<AssetItem> g_assets;

// ------------------------------- Utilities -------------------------------
std::string GetExeFolderA() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p(buf);
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) return p.substr(0, pos);
    return p;
}

std::string JoinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "\\" + b;
}

std::string ReadFileText(const std::string &path) {
    std::ifstream fi(path);
    if (!fi) return {};
    std::ostringstream ss; ss << fi.rdbuf();
    return ss.str();
}

bool WriteFileText(const std::string &path, const std::string &text) {
    std::ofstream fo(path);
    if (!fo) return false;
    fo << text;
    return true;
}

// Very small, tolerant JSON-ish parser for our simple scene format
void LoadSceneFromFile(const std::string &scenePath) {
    g_entities.clear();
    std::string txt = ReadFileText(scenePath);
    if (txt.empty()) {
        AppendLog("Scene file empty or missing: " + scenePath);
        return;
    }

    // naive parse: look for occurrences of { "id" : "...", ... }
    size_t pos = 0;
    while (true) {
        pos = txt.find('{', pos);
        if (pos == std::string::npos) break;
        size_t end = txt.find('}', pos);
        if (end == std::string::npos) break;
        std::string block = txt.substr(pos, end - pos + 1);
        pos = end + 1;

        if (block.find("\"id\"") == std::string::npos) continue;
        Entity e;
        // id
        auto findStr = [&](const std::string &key)->std::string{
            size_t p = block.find(key);
            if (p==std::string::npos) return "";
            size_t q = block.find('"', p + key.size());
            if (q==std::string::npos) return "";
            size_t r = block.find('"', q+1);
            if (r==std::string::npos) return "";
            return block.substr(q+1, r-q-1);
        };
        e.id = findStr("\"id\"");
        e.asset = findStr("\"asset\"");
        // ints: x,y,z
        auto findInt = [&](const std::string &key, int def)->int{
            size_t p = block.find(key);
            if (p==std::string::npos) return def;
            size_t colon = block.find(':', p);
            if (colon==std::string::npos) return def;
            size_t start = colon+1;
            // skip spaces
            while (start < block.size() && isspace((unsigned char)block[start])) start++;
            int sign = 1;
            if (block[start] == '-') { sign = -1; start++; }
            int val = 0;
            bool any=false;
            while (start < block.size() && isdigit((unsigned char)block[start])) { any=true; val = val*10 + (block[start]-'0'); start++; }
            return any ? val*sign : def;
        };
        e.x = findInt("\"x\"", 0);
        e.y = findInt("\"y\"", 0);
        e.z = findInt("\"z\"", 0);
        g_entities.push_back(e);
    }
    AppendLog("Loaded entities count: " + std::to_string(g_entities.size()));
}

void SaveSceneToFile(const std::string &scenePath) {
    std::ostringstream ss;
    ss << "{\n  \"id\": \"main\",\n  \"entities\": [\n";
    for (size_t i=0;i<g_entities.size();++i) {
        auto &e = g_entities[i];
        ss << "    { \"id\": \"" << e.id << "\", \"asset\": \"" << e.asset << "\", \"x\": " << e.x << ", \"y\": " << e.y << ", \"z\": " << e.z << " }";
        if (i+1<g_entities.size()) ss << ",\n"; else ss << "\n";
    }
    ss << "  ],\n  \"script\": []\n}\n";
    bool ok = WriteFileText(scenePath, ss.str());
    AppendLog(std::string("Saved scene: ") + (ok?"OK":"FAILED") );
}

// ------------------------------- Asset scanning & loading -------------------------------
void UnloadAssets() {
    for (auto &a : g_assets) {
        if (a.bmp) delete a.bmp;
        a.bmp = nullptr;
    }
    g_assets.clear();
}

Bitmap* LoadBitmapSafe(const std::string &path) {
    // GDI+ can take wchar_t filename (wide string). Convert.
    std::wstring w(path.begin(), path.end());
    Bitmap *b = new Bitmap(w.c_str());
    if (b->GetLastStatus() != Ok) {
        delete b; return nullptr;
    }
    return b;
}

void ScanAssets() {
    UnloadAssets();
    if (g_assetsPath.empty()) return;
    try {
        for (auto &p : fs::directory_iterator(g_assetsPath)) {
            if (!p.is_regular_file()) continue;
            std::string fn = p.path().filename().string();
            // accept .bmp, .png, .jpg, .jpeg, .wav etc
            std::string low = fn;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if (low.find(".bmp") == std::string::npos && low.find(".png") == std::string::npos && low.find(".jpg") == std::string::npos && low.find(".jpeg") == std::string::npos) continue;
            AssetItem ai; ai.filename = fn; ai.bmp = nullptr;
            std::string full = JoinPath(g_assetsPath, fn);
            ai.bmp = LoadBitmapSafe(full);
            if (ai.bmp) {
                ai.w = ai.bmp->GetWidth(); ai.h = ai.bmp->GetHeight();
            }
            g_assets.push_back(ai);
            AppendLog("Found asset: " + fn + (ai.bmp?" (loaded)":" (failed)"));
        }
    } catch (...) {
        AppendLog("ScanAssets: exception scanning assets folder");
    }
}

Bitmap* FindAssetBitmap(const std::string &filename) {
    for (auto &a : g_assets) if (a.filename == filename) return a.bmp;
    return nullptr;
}

// After assets loaded, assign bitmaps to entities
void AttachAssetsToEntities() {
    for (auto &e : g_entities) {
        e.bmp = FindAssetBitmap(e.asset);
    }
}

// ------------------------------- UI / Win32 -------------------------------
static HWND g_hWnd = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static int g_winW = 1024, g_winH = 720;

// UI state
static int g_toolbarH = 36;
static int g_leftW = 240;
static int g_rightW = 280;
static int g_sceneLeft = 0;
static int g_sceneTop = 0;
static int g_sceneW = 0;
static int g_sceneH = 0;

// Interaction
static int g_selectedEntity = -1; // index
static bool g_dragging = false;
static int g_dragOffsetX = 0, g_dragOffsetY = 0;

// Basic grid
static bool g_showGrid = true;
static int g_gridStep = 32;

// Read / Write config
bool ReadConfig() {
    std::string cfgPath = JoinPath(g_exeFolder, "config.txt");
    g_logPath = JoinPath(g_exeFolder, "editor.log");
    AppendLog("--- Thunderz Editor startup ---");
    AppendLog("EXE folder: " + g_exeFolder);
    std::ifstream fi(cfgPath);
    if (!fi) {
        AppendLog("Config not found, using defaults");
        // default: project under exe/Projects/ExampleProject
        g_projectPath = JoinPath(g_exeFolder, "Projects\\ExampleProject");
    } else {
        std::string line;
        while (std::getline(fi, line)) {
            if (line.find("project_path=") == 0) {
                std::string v = line.substr(strlen("project_path="));
                // normalize slashes
                for (auto &c : v) if (c=='/') c='\\';
                g_projectPath = v;
            }
        }
    }
    if (g_projectPath.empty()) {
        AppendLog("project_path empty");
        return false;
    }
    g_assetsPath = JoinPath(g_projectPath, "assets");
    g_scenesPath = JoinPath(g_projectPath, "scenes");
    AppendLog("Resolved project path: " + g_projectPath);
    return true;
}

// Simple paint helpers
void FillRect(Graphics &g, int x, int y, int w, int h, const Color &c) {
    SolidBrush br(c);
    g.FillRectangle(&br, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
}

void DrawStringLeft(Graphics &g, const std::wstring &s, Font &f, int x, int y, Brush &brush) {
    PointF pt((REAL)x, (REAL)y);
    g.DrawString(s.c_str(), -1, &f, pt, &brush);
}

// Core drawing
void DrawGrid(Graphics &g, int left, int top, int w, int h) {
    if (!g_showGrid) return;
    Pen pen(Color(80, 200,200,200));
    for (int x = left; x < left + w; x += g_gridStep) g.DrawLine(&pen, (REAL)x, (REAL)top, (REAL)x, (REAL)(top+h));
    for (int y = top; y < top + h; y += g_gridStep) g.DrawLine(&pen, (REAL)left, (REAL)y, (REAL)(left+w), (REAL)y);
}

void DrawScene(Graphics &g) {
    // scene rect
    int left = g_leftW;
    int top = g_toolbarH;
    int w = g_winW - g_leftW - g_rightW;
    int h = g_winH - g_toolbarH;
    g_sceneLeft = left; g_sceneTop = top; g_sceneW = w; g_sceneH = h;

    // background
    FillRect(g, left, top, w, h, Color(255,24,24,24));
    DrawGrid(g, left, top, w, h);

    // draw entities sorted by z
    std::vector<int> order;
    for (size_t i=0;i<g_entities.size();++i) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b){ return g_entities[a].z < g_entities[b].z; });

    for (int idx : order) {
        Entity &e = g_entities[idx];
        int ex = left + e.x;
        int ey = top + e.y;
        if (e.bmp) {
            // draw centered on its x,y (treat x,y as top-left to keep simple)
            Bitmap *b = e.bmp;
            int bw = b->GetWidth();
            int bh = b->GetHeight();
            g.DrawImage(b, (REAL)ex, (REAL)ey, (REAL)bw, (REAL)bh);
        } else {
            // placeholder
            SolidBrush brush(Color(255, 120,120,120));
            Pen pen(Color(255,255,255,255));
            g.FillRectangle(&brush, (REAL)ex, (REAL)ey, 48.0f, 48.0f);
            g.DrawRectangle(&pen, (REAL)ex, (REAL)ey, 48.0f, 48.0f);
        }
        // selection highlight
        if ((int)idx == g_selectedEntity) {
            Pen sPen(Color(255,0,200,255), 2.0f);
            g.DrawRectangle(&sPen, (REAL)ex, (REAL)ey,  (REAL)(e.bmp?e.bmp->GetWidth():48), (REAL)(e.bmp?e.bmp->GetHeight():48));
        }
    }
}

void DrawSidebarLeft(Graphics &g) {
    // left panel background
    FillRect(g, 0, g_toolbarH, g_leftW, g_winH - g_toolbarH, Color(255,30,30,30));
    Font font(L"Segoe UI", 10);
    SolidBrush white(Color(255,240,240,240));
    DrawStringLeft(g, L"Assets", font, 8, g_toolbarH + 8, white);

    // list thumbnails
    int y = g_toolbarH + 36;
    int thumbH = 56; int thumbX = 8;
    for (size_t i=0;i<g_assets.size();++i) {
        AssetItem &ai = g_assets[i];
        // thumbnail box
        RectF dst((REAL)thumbX, (REAL)y, 56.0f, 56.0f);
        if (ai.bmp) {
            REAL iw = (REAL)ai.bmp->GetWidth(); REAL ih = (REAL)ai.bmp->GetHeight();
            float scale = std::min(56.0f/iw, 56.0f/ih);
            REAL dw = iw * scale, dh = ih * scale;
            REAL dx = (56.0f - dw)/2.0f + thumbX;
            REAL dy = (56.0f - dh)/2.0f + y;
            g.DrawImage(ai.bmp, dx, dy, dw, dh);
        } else {
            FillRect(g, thumbX, y, 56, 56, Color(255,80,80,80));
        }
        // name
        std::wstring nameW(ai.filename.begin(), ai.filename.end());
        DrawStringLeft(g, nameW, font, 8+64, y+20, white);
        y += thumbH + 8;
    }
}

void DrawInspectorRight(Graphics &g) {
    int left = g_winW - g_rightW;
    FillRect(g, left, g_toolbarH, g_rightW, g_winH - g_toolbarH, Color(255,28,28,28));
    Font font(L"Segoe UI", 10);
    SolidBrush white(Color(255,240,240,240));
    DrawStringLeft(g, L"Inspector", font, left + 8, g_toolbarH + 8, white);

    int y = g_toolbarH + 36;
    if (g_selectedEntity >= 0 && g_selectedEntity < (int)g_entities.size()) {
        Entity &e = g_entities[g_selectedEntity];
        std::wstring idw(e.id.begin(), e.id.end());
        DrawStringLeft(g, idw, font, left + 8, y, white); y += 24;

        // position
        std::wstring pos = L"X: " + std::to_wstring(e.x) + L"   Y: " + std::to_wstring(e.y) + L"   Z: " + std::to_wstring(e.z);
        DrawStringLeft(g, pos, font, left + 8, y, white); y += 24;

        // small preview
        if (e.bmp) {
            g.DrawImage(e.bmp, (REAL)(left+8), (REAL)y, 96.0f, 96.0f);
            y += 100;
        }
    } else {
        DrawStringLeft(g, L"No selection", font, left+8, y, white);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        g_winW = LOWORD(lParam); g_winH = HIWORD(lParam);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        // inside left assets?
        if (mx < g_leftW && my > g_toolbarH) {
            // clicked on asset list -> add entity to scene
            int index = (my - (g_toolbarH + 36)) / (56 + 8);
            if (index >= 0 && index < (int)g_assets.size()) {
                // create new entity
                Entity e;
                e.id = "entity" + std::to_string(g_entities.size()+1);
                e.asset = g_assets[index].filename;
                e.x = 20; e.y = 20; e.z = 0;
                e.bmp = g_assets[index].bmp;
                g_entities.push_back(e);
                AppendLog("Added entity: " + e.id + " asset=" + e.asset);
                InvalidateRect(hWnd, NULL, TRUE);
            }
            return 0;
        }
        // inside scene area? select entity
        if (mx >= g_sceneLeft && mx <= g_sceneLeft + g_sceneW && my >= g_sceneTop && my <= g_sceneTop + g_sceneH) {
            int sx = mx - g_sceneLeft, sy = my - g_sceneTop;
            // iterate entities reverse z
            int found = -1;
            for (int i=(int)g_entities.size()-1;i>=0;--i) {
                Entity &e = g_entities[i];
                int ex = e.x, ey = e.y;
                int ew = e.bmp?e.bmp->GetWidth():48;
                int eh = e.bmp?e.bmp->GetHeight():48;
                if (sx >= ex && sx <= ex + ew && sy >= ey && sy <= ey + eh) { found = i; break; }
            }
            g_selectedEntity = found;
            if (found >= 0) {
                g_dragging = true;
                g_dragOffsetX = sx - g_entities[found].x;
                g_dragOffsetY = sy - g_entities[found].y;
            }
            InvalidateRect(hWnd, NULL, TRUE);
            return 0;
        }
        break; }
    case WM_MOUSEMOVE: {
        if (g_dragging && g_selectedEntity >= 0 && g_selectedEntity < (int)g_entities.size()) {
            int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            int sx = mx - g_sceneLeft, sy = my - g_sceneTop;
            g_entities[g_selectedEntity].x = sx - g_dragOffsetX;
            g_entities[g_selectedEntity].y = sy - g_dragOffsetY;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break; }
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false; AppendLog("End drag");
            InvalidateRect(hWnd, NULL, TRUE);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == 'S') {
            // save scene
            std::string sceneFile = JoinPath(g_scenesPath, "main.scene");
            SaveSceneToFile(sceneFile);
        } else if (wParam == 'P') {
            // launch runtime (assumed in same folder as exe)
            std::string runtimePath = JoinPath(g_exeFolder, "ThunderzRuntime.exe");
            ShellExecuteA(NULL, "open", runtimePath.c_str(), NULL, g_exeFolder.c_str(), SW_SHOWNORMAL);
            AppendLog("Launched runtime: " + runtimePath);
        } else if (wParam == 'G') {
            g_showGrid = !g_showGrid; InvalidateRect(hWnd, NULL, TRUE);
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(255,18,18,18));

        // toolbar
        FillRect(g, 0, 0, g_winW, g_toolbarH, Color(255,40,40,40));
        Font font(L"Segoe UI", 11);
        SolidBrush white(Color(255,240,240,240));
        DrawStringLeft(g, L"Thunderz Editor - Advanced (stable build)", font, 8, 6, white);

        // draw left, scene, right
        DrawSidebarLeft(g);
        DrawScene(g);
        DrawInspectorRight(g);

        EndPaint(hWnd, &ps);
        break; }
    case WM_DESTROY:
        PostQuitMessage(0); break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // initialize exe folder and log
    g_exeFolder = GetExeFolderA();
    g_logPath = JoinPath(g_exeFolder, "editor.log");
    // truncate log
    {
        std::ofstream lf(g_logPath, std::ios::trunc);
        if (lf) lf << "";
    }
    AppendLog("Starting editor...");

    // GDI+
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL) != Ok) {
        MessageBoxA(NULL, "Failed to init GDI+", "Error", MB_OK);
        return 1;
    }
    AppendLog("GDI+ initialized");

    // read config
    ReadConfig();

    // scan assets and scenes
    ScanAssets();
    std::string sceneFile = JoinPath(g_scenesPath, "main.scene");
    if (!fs::exists(sceneFile)) {
        AppendLog("main.scene missing or unreadable");
    } else {
        AppendLog("Loading scene from: " + sceneFile);
        LoadSceneFromFile(sceneFile);
    }
    AttachAssetsToEntities();

    // Register window
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ThunderzEditorClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClass(&wc);

    RECT r = {0,0,g_winW,g_winH}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hWnd = CreateWindowEx(0, wc.lpszClassName, L"Thunderz Editor", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, g_winW, g_winH, NULL, NULL, hInstance, NULL);

    if (!g_hWnd) { AppendLog("Failed to create window"); return 1; }
    ShowWindow(g_hWnd, nCmdShow);

    // message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    AppendLog("Editor exiting");
    return 0;
}


