// ThunderzEditor.cpp
// Minimal Thunderz Editor with scene loading, drag, and inspector editing.
// Build with: g++ -static -O2 -std=gnu++17 ThunderzEditor.cpp -o ThunderzEditor.exe -lgdiplus -lgdi32 -luser32 -lkernel32 -municode

#define UNICODE
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <codecvt>
#include <locale>
#include <chrono>

using namespace Gdiplus;
namespace fs = std::filesystem;

// Simple logger
void Log(const std::wstring &s) {
    std::wofstream f("editor.log", std::ios::app);
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::wstring timew(64, L'\0');
    std::wstringstream ss; ss << std::put_time(std::localtime(&t), L"%Y-%m-%d %H:%M:%S");
    std::wstring line = ss.str() + L" - " + s + L"\n";
    f << line;
    f.close();
}

// Helper: convert narrow -> wide and vice versa
std::wstring s2ws(const std::string &s) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(s);
}
std::string ws2s(const std::wstring &w) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.to_bytes(w);
}

// Entity representation
struct Entity {
    std::string id;
    std::string asset; // filename only
    int x=0, y=0, z=0;
    int w=48, h=48;
    Bitmap* bmp = nullptr; // owned by editor (not deleted when removed)
};

// Globals
HINSTANCE g_hInst = NULL;
HWND g_hWnd = NULL;
int g_winW = 1280, g_winH = 720;
std::wstring g_exeFolder;
std::wstring g_projectPath;
std::vector<std::pair<std::string, Bitmap*>> g_assets; // filename -> Bitmap*
std::vector<Entity> g_entities;
int g_selectedIndex = -1;
bool g_documentDirty = false;

// UI panes sizes
int leftPanelW = 160;
int inspectorW = 320;
int bottomPanelH = 140;
int toolbarH = 48;

// Inspector controls
HWND hEditX = NULL, hEditY = NULL, hEditZ = NULL;
HWND hBtnApply = NULL;

// Dragging state
bool g_isDragging = false;
int g_dragIndex = -1;
int g_dragOffsetX = 0;
int g_dragOffsetY = 0;

// Forward
void RepaintWindow();
void UpdateInspectorFields();
int HitTestEntity(int px, int py);
void SaveScene();

std::wstring ReadAllTextW(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return L"";
    std::ostringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    return s2ws(s);
}

std::string ReadAllTextA(const fs::path &p) {
    std::ifstream f(p);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Very simple scene parser for the specific format (no dependencies)
void LoadSceneFile(const fs::path &scenePath) {
    g_entities.clear();
    if (!fs::exists(scenePath)) {
        Log(L"LoadSceneFile: scene file missing: " + scenePath.native());
        return;
    }
    std::string content = ReadAllTextA(scenePath);
    // naive parse: find occurrences of {"id":...,"asset":...,"x":..., "y":..., "z":...}
    size_t pos = 0;
    while (true) {
        size_t brace = content.find('{', pos);
        if (brace == std::string::npos) break;
        size_t end = content.find('}', brace);
        if (end == std::string::npos) break;
        std::string block = content.substr(brace, end - brace + 1);
        // find id
        auto getStr = [&](const std::string &key)->std::string {
            size_t p = block.find("\"" + key + "\"");
            if (p==std::string::npos) return "";
            size_t colon = block.find(':', p);
            if (colon==std::string::npos) return "";
            size_t quote = block.find('"', colon);
            if (quote==std::string::npos) return "";
            size_t quote2 = block.find('"', quote+1);
            if (quote2==std::string::npos) return "";
            return block.substr(quote+1, quote2-quote-1);
        };
        auto getInt = [&](const std::string &key, int def)->int {
            size_t p = block.find("\"" + key + "\"");
            if (p==std::string::npos) return def;
            size_t colon = block.find(':', p);
            if (colon==std::string::npos) return def;
            size_t start = colon+1;
            while (start<block.size() && (block[start]==' ' || block[start]=='\t')) ++start;
            size_t finish = start;
            while (finish<block.size() && ( (block[finish]>='0' && block[finish]<='9') || block[finish]=='-' )) finish++;
            if (finish==start) return def;
            std::string num = block.substr(start, finish-start);
            try { return stoi(num); } catch(...) { return def; }
        };
        std::string id = getStr("id");
        std::string asset = getStr("asset");
        int x = getInt("x", 0);
        int y = getInt("y", 0);
        int z = getInt("z", 0);
        // if it has id or asset, treat as entity
        if (!id.empty() || !asset.empty()) {
            Entity e;
            e.id = id.empty() ? "entity" : id;
            e.asset = asset;
            e.x = x; e.y = y; e.z = z;
            // lookup loaded bitmap to set w/h later
            e.bmp = nullptr;
            for (auto &p : g_assets) {
                if (p.first == e.asset) { e.bmp = p.second; break; }
            }
            if (e.bmp) {
                e.w = (int)e.bmp->GetWidth();
                e.h = (int)e.bmp->GetHeight();
            }
            g_entities.push_back(e);
        }
        pos = end + 1;
    }
    Log(L"Loaded entities count: " + std::to_wstring(g_entities.size()));
}

void ScanAssets() {
    g_assets.clear();
    fs::path assetsFolder = fs::path(g_projectPath) / "assets";
    if (!fs::exists(assetsFolder)) {
        Log(L"ScanAssets: assets folder missing: " + assetsFolder.native());
        return;
    }
    for (auto &it : fs::directory_iterator(assetsFolder)) {
        if (!it.is_regular_file()) continue;
        std::string name = it.path().filename().u8string();
        std::string ext = it.path().extension().u8string();
        // support .bmp only for now
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".bmp" || ext == ".png" || ext == ".jpg") {
            // load via Gdiplus
            std::wstring full = it.path().wstring();
            Bitmap* bmp = Bitmap::FromFile(full.c_str());
            if (bmp && bmp->GetLastStatus() == Ok) {
                g_assets.emplace_back(name, bmp);
                std::wstring msg = L"Found asset: " + it.path().wstring();
                Log(msg);
            } else {
                if (bmp) delete bmp;
                Log(L"Failed to load asset: " + it.path().wstring());
            }
        }
    }
}

// Save scene in the simple format
void SaveSceneFile(const fs::path &scenePath) {
    std::ofstream f(scenePath);
    if (!f) {
        Log(L"SaveSceneFile: failed to open " + scenePath.native());
        return;
    }
    f << "{\n  \"id\":\"main\",\n  \"entities\":[\n";
    for (size_t i=0;i<g_entities.size();++i) {
        auto &e = g_entities[i];
        f << "    { \"id\":\"" << e.id << "\", \"asset\":\"" << e.asset << "\", \"x\":" << e.x << ", \"y\":" << e.y << ", \"z\":" << e.z << " }";
        if (i+1<g_entities.size()) f << ",";
        f << "\n";
    }
    f << "  ],\n  \"script\":[]\n}\n";
    f.close();
    Log(L"Saved scene: " + scenePath.native());
    g_documentDirty = false;
}

// UI helpers
RECT GetSceneRect() {
    RECT r; r.left = leftPanelW; r.top = toolbarH; r.right = g_winW - inspectorW; r.bottom = g_winH - bottomPanelH;
    return r;
}
RECT GetHierarchyRect() {
    RECT r; r.left = 0; r.top = toolbarH; r.right = leftPanelW; r.bottom = g_winH - bottomPanelH;
    return r;
}
RECT GetInspectorRect() {
    RECT r; r.left = g_winW - inspectorW; r.top = toolbarH; r.right = g_winW; r.bottom = g_winH - bottomPanelH;
    return r;
}
RECT GetBottomRect() {
    RECT r; r.left = 0; r.top = g_winH - bottomPanelH; r.right = g_winW - inspectorW; r.bottom = g_winH;
    return r;
}

// Update the three edit boxes from selection
void UpdateInspectorFields() {
    if (!hEditX || !hEditY || !hEditZ) return;
    if (g_selectedIndex >= 0 && g_selectedIndex < (int)g_entities.size()) {
        Entity &e = g_entities[g_selectedIndex];
        std::wstring sx = std::to_wstring(e.x);
        std::wstring sy = std::to_wstring(e.y);
        std::wstring sz = std::to_wstring(e.z);
        SetWindowTextW(hEditX, sx.c_str());
        SetWindowTextW(hEditY, sy.c_str());
        SetWindowTextW(hEditZ, sz.c_str());
    } else {
        SetWindowTextW(hEditX, L"");
        SetWindowTextW(hEditY, L"");
        SetWindowTextW(hEditZ, L"");
    }
}

// Hit test: returns topmost entity index under point px,py (scene coords)
int HitTestEntity(int px, int py) {
    // scene rect offset
    RECT srect = GetSceneRect();
    int sx = srect.left, sy = srect.top;
    for (int i=(int)g_entities.size()-1;i>=0;--i) {
        Entity &e = g_entities[i];
        int ex = sx + e.x;
        int ey = sy + e.y;
        int ew = e.w;
        int eh = e.h;
        if (px >= ex && px <= ex + ew && py >= ey && py <= ey + eh) return i;
    }
    return -1;
}

// apply values from edit boxes to selected entity
void ApplyInspectorToEntity() {
    if (g_selectedIndex < 0 || g_selectedIndex >= (int)g_entities.size()) return;
    wchar_t buf[64];
    GetWindowTextW(hEditX, buf, 64);
    int nx= g_entities[g_selectedIndex].x;
    try { nx = std::stoi(ws2s(std::wstring(buf))); } catch(...) {}
    GetWindowTextW(hEditY, buf, 64);
    int ny = g_entities[g_selectedIndex].y;
    try { ny = std::stoi(ws2s(std::wstring(buf))); } catch(...) {}
    GetWindowTextW(hEditZ, buf, 64);
    int nz = g_entities[g_selectedIndex].z;
    try { nz = std::stoi(ws2s(std::wstring(buf))); } catch(...) {}
    g_entities[g_selectedIndex].x = nx;
    g_entities[g_selectedIndex].y = ny;
    g_entities[g_selectedIndex].z = nz;
    g_documentDirty = true;
    RepaintWindow();
}

// Launch runtime
void LaunchRuntime() {
    fs::path exe = fs::path(g_exeFolder) / "ThunderzRuntime.exe";
    if (!fs::exists(exe)) {
        Log(L"Runtime exe not found: " + exe.native());
        return;
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    std::wstring cmd = exe.wstring();
    if (CreateProcessW(cmd.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        Log(L"Launched runtime: " + exe.native());
    } else {
        Log(L"CreateProcess failed for runtime.");
    }
}

// Drawing
void PaintScene(Gdiplus::Graphics &g) {
    RECT r = GetSceneRect();
    int sx = r.left, sy = r.top;
    // background checkerboard
    SolidBrush bgBrush(Color(0xFF, 0x2e,0x2f,0x33)); // dark
    g.FillRectangle(&bgBrush, (REAL)r.left, (REAL)r.top, (REAL)(r.right-r.left), (REAL)(r.bottom-r.top));
    // grid lines
    Pen gridPen(Color(60,80,90,100));
    int grid = 32;
    for (int x = r.left; x < r.right; x+=grid) g.DrawLine(&gridPen, (REAL)x, (REAL)r.top, (REAL)x, (REAL)r.bottom);
    for (int y = r.top; y < r.bottom; y+=grid) g.DrawLine(&gridPen, (REAL)r.left, (REAL)y, (REAL)r.right, (REAL)y);
    // draw entities sorted by z (ascending)
    std::vector<int> order;
    for (size_t i=0;i<g_entities.size();++i) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b){
        if (g_entities[a].z == g_entities[b].z) return a < b;
        return g_entities[a].z < g_entities[b].z;
    });
    for (int idx : order) {
        Entity &e = g_entities[idx];
        int ex = sx + e.x;
        int ey = sy + e.y;
        if (e.bmp) {
            g.DrawImage(e.bmp, ex, ey, e.w, e.h);
        } else {
            SolidBrush b(Color(0xFF, 0xC9,0x55,0x55));
            g.FillRectangle(&b, (REAL)ex, (REAL)ey, (REAL)48, (REAL)48);
            FontFamily fontFamily(L"Arial");
            Gdiplus::Font font(&fontFamily, 10, FontStyleRegular, UnitPixel);
            SolidBrush tb(Color::White);
            std::wstring label = s2ws(e.id.empty() ? "entity" : e.id);
            g.DrawString(label.c_str(), -1, &font, PointF((REAL)ex+6,(REAL)ey+12), &tb);
        }
        // draw small label in red on top-left of scene (entity id)
        if (idx == g_selectedIndex) {
            Pen sel(Color::Red);
            sel.SetWidth(2.0f);
            g.DrawRectangle(&sel, (REAL)ex-2, (REAL)ey-2, (REAL)e.w+4, (REAL)e.h+4);
        }
    }
}

// repaint wrapper
void RepaintWindow() {
    if (g_hWnd) InvalidateRect(g_hWnd, NULL, TRUE);
}

// WndProc
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hwnd;
        // create inspector edit controls within inspector rect
        RECT ir = GetInspectorRect();
        int ix = ir.left + 12, iy = ir.top + 24;
        // create labels via Draw in WM_PAINT, create controls
        hEditX = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 ix, iy, 80, 24, hwnd, (HMENU)1001, g_hInst, NULL);
        hEditY = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 ix+92, iy, 80, 24, hwnd, (HMENU)1002, g_hInst, NULL);
        hEditZ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 ix+184, iy, 80, 24, hwnd, (HMENU)1003, g_hInst, NULL);
        hBtnApply = CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  ix+276, iy, 64, 24, hwnd, (HMENU)2001, g_hInst, NULL);
        UpdateInspectorFields();
        return 0;
    }
    case WM_SIZE: {
        g_winW = LOWORD(lParam);
        g_winH = HIWORD(lParam);
        RepaintWindow();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        // click inside scene?
        RECT srect = GetSceneRect();
        if (mx >= srect.left && mx < srect.right && my >= srect.top && my < srect.bottom) {
            int idx = HitTestEntity(mx, my);
            if (idx >= 0) {
                g_selectedIndex = idx;
                g_isDragging = true;
                g_dragIndex = idx;
                g_dragOffsetX = mx - (srect.left + g_entities[idx].x);
                g_dragOffsetY = my - (srect.top + g_entities[idx].y);
                SetCapture(hwnd);
                UpdateInspectorFields();
                RepaintWindow();
            } else {
                g_selectedIndex = -1;
                UpdateInspectorFields();
                RepaintWindow();
            }
        } else {
            // check click on hierarchy (left panel) — simple hit by items
            RECT hrect = GetHierarchyRect();
            if (mx >= hrect.left && mx < hrect.right && my >= hrect.top && my < hrect.bottom) {
                // compute clicked index in list
                int itemH = 28;
                int relY = my - hrect.top - 20;
                if (relY >= 0) {
                    int idx = relY / itemH;
                    if (idx >= 0 && idx < (int)g_entities.size()) {
                        g_selectedIndex = idx;
                        UpdateInspectorFields();
                        RepaintWindow();
                    }
                }
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_isDragging && g_dragIndex >= 0) {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            RECT srect = GetSceneRect();
            int nx = mx - srect.left - g_dragOffsetX;
            int ny = my - srect.top - g_dragOffsetY;
            g_entities[g_dragIndex].x = nx;
            g_entities[g_dragIndex].y = ny;
            g_documentDirty = true;
            UpdateInspectorFields();
            RepaintWindow();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_isDragging) {
            g_isDragging = false;
            g_dragIndex = -1;
            ReleaseCapture();
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == 2001 && code == BN_CLICKED) {
            // Apply button
            ApplyInspectorToEntity();
            return 0;
        }
        if ((id == 1001 || id == 1002 || id == 1003) && code == EN_KILLFOCUS) {
            // editing finished -> immediate apply
            ApplyInspectorToEntity();
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Ctrl+S
            SaveScene();
            return 0;
        }
        if (wParam == 'P') {
            LaunchRuntime();
            return 0;
        }
        if (wParam == VK_F5) {
            LaunchRuntime();
            return 0;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // Clear whole background
        SolidBrush appBg(Color(0xFF, 0x2B, 0x2C, 0x31));
        g.FillRectangle(&appBg, 0, 0, g_winW, g_winH);
        // Toolbar
        SolidBrush tb(Color(0xFF,0x28,0x2C,0x32));
        g.FillRectangle(&tb, 0, 0, g_winW, toolbarH);
        FontFamily ff(L"Segoe UI");
        Font font(&ff, 14, FontStyleRegular, UnitPixel);
        SolidBrush white(Color::White);
        g.DrawString(L"Thunderz Engine - Editor", -1, &font, PointF(8,12), &white);
        // draw Save and Play buttons visually (functionality via keys and controls)
        // left panel (hierarchy)
        RECT hrect = GetHierarchyRect();
        SolidBrush leftBg(Color(0xFF, 0x22,0x23,0x27));
        g.FillRectangle(&leftBg, (REAL)hrect.left, (REAL)hrect.top, (REAL)(hrect.right-hrect.left), (REAL)(hrect.bottom-hrect.top));
        // draw list of entities
        Font fontSmall(&ff, 12, FontStyleRegular, UnitPixel);
        SolidBrush itemSel(Color(0xFF,0x33,0x3A,0x4B));
        SolidBrush itemTxt(Color(0xFF,0xDD,0xDD,0xDD));
        int itemY = hrect.top + 20;
        int itemH = 28;
        g.DrawString(L"Hierarchy", -1, &fontSmall, PointF((REAL)hrect.left+8, (REAL)(hrect.top+4)), &itemTxt);
        for (size_t i=0;i<g_entities.size();++i) {
            RECT ir = {hrect.left+6, itemY, hrect.right-6, itemY + itemH};
            if ((int)i == g_selectedIndex) {
                g.FillRectangle(&itemSel, (REAL)ir.left, (REAL)ir.top, (REAL)(ir.right-ir.left), (REAL)(ir.bottom-ir.top));
            }
            std::wstring label = s2ws(g_entities[i].id.empty() ? "entity" : g_entities[i].id);
            g.DrawString(label.c_str(), -1, &fontSmall, PointF((REAL)ir.left+6,(REAL)ir.top+6), &white);
            itemY += itemH;
        }
        // scene
        PaintScene(g);
        // inspector right
        RECT ir = GetInspectorRect();
        SolidBrush inspBg(Color(0xFF,0x18,0x19,0x1C));
        g.FillRectangle(&inspBg, (REAL)ir.left, (REAL)ir.top, (REAL)(ir.right-ir.left), (REAL)(ir.bottom-ir.top));
        g.DrawString(L"Inspector", -1, &fontSmall, PointF((REAL)ir.left+8,(REAL)ir.top+6), &itemTxt);
        // draw labels for X Y Z near the edit controls (we placed them at WM_CREATE)
        g.DrawString(L"X:", -1, &fontSmall, PointF((REAL)ir.left+8,(REAL)ir.top+30), &itemTxt);
        g.DrawString(L"Y:", -1, &fontSmall, PointF((REAL)ir.left+100,(REAL)ir.top+30), &itemTxt);
        g.DrawString(L"Z:", -1, &fontSmall, PointF((REAL)ir.left+192,(REAL)ir.top+30), &itemTxt);
        // bottom assets area
        RECT brect = GetBottomRect();
        SolidBrush bbg(Color(0xFF,0x16,0x17,0x1A));
        g.FillRectangle(&bbg, (REAL)brect.left, (REAL)brect.top, (REAL)(brect.right-brect.left), (REAL)(brect.bottom-brect.top));
        g.DrawString(L"Assets", -1, &fontSmall, PointF((REAL)brect.left+8,(REAL)brect.top+6), &itemTxt);
        // draw thumbnails
        int tx = brect.left + 8;
        int ty = brect.top + 30;
        for (auto &p : g_assets) {
            Bitmap* bm = p.second;
            if (!bm) continue;
            int thumb = 64;
            g.DrawImage(bm, tx, ty, thumb, thumb);
            std::wstring nm = s2ws(p.first);
            g.DrawString(nm.c_str(), -1, &fontSmall, PointF((REAL)tx+4,(REAL)ty+thumb+4), &itemTxt);
            tx += thumb + 12;
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: {
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Save helper triggered by Ctrl+S or Save control
void SaveScene() {
    fs::path scenePath = fs::path(g_projectPath) / "scenes" / "main.scene";
    SaveSceneFile(scenePath);
}

// Reads config.txt
void ReadConfig() {
    fs::path cfg = fs::path(g_exeFolder) / "config.txt";
    if (!fs::exists(cfg)) {
        Log(L"config.txt missing in exe folder.");
        return;
    }
    std::ifstream f(cfg);
    std::string line;
    while (std::getline(f, line)) {
        // trim
        while (!line.empty() && (line.back()=='\r' || line.back()=='\n')) line.pop_back();
        if (line.rfind("project_path=",0) == 0) {
            std::string val = line.substr(strlen("project_path="));
            // convert to wide path
            g_projectPath = s2ws(val);
            Log(L"Read config: project_path=" + g_projectPath);
        }
    }
    f.close();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;
    // clear log
    std::wofstream fl("editor.log");
    fl << L"";
    fl.close();
    // determine exe folder
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    fs::path exePath = fs::path(buf);
    g_exeFolder = exePath.parent_path().wstring();
    Log(L"--- Thunderz Editor (debug) startup ---");
    Log(L"EXE folder: " + g_exeFolder);
    // read config.txt
    ReadConfig();
    if (g_projectPath.empty()) {
        Log(L"No project path in config.txt. Please set project_path=...");
    }
    // init GDI+
    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    Log(L"GDI+ initialized");
    // scan assets
    ScanAssets();
    // load scene
    fs::path scenePath = fs::path(g_projectPath) / "scenes" / "main.scene";
    LoadSceneFile(scenePath);
    // window class
    const wchar_t CLASS_NAME[] = L"ThunderzEditorWindowClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    // create window
    g_hWnd = CreateWindowExW(0, CLASS_NAME, L"Thunderz Editor", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, g_winW, g_winH,
                             NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        Log(L"CreateWindowEx failed");
        return 0;
    }
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    // message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // cleanup assets
    for (auto &p : g_assets) { if (p.second) delete p.second; }
    GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}


