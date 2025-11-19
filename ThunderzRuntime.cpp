// ThunderzRuntime.cpp
// Minimal runtime that loads config -> project -> scene and assets.
// Compiles with MinGW + Gdiplus.
// Build:
// g++ -static -O2 -std=gnu++17 ThunderzRuntime.cpp -o build/ThunderzRuntime.exe -lgdiplus -lgdi32 -luser32 -lkernel32 -municode -mwindows

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <codecvt>
#include <locale>
#include <ctime>
#include <algorithm> // <--- FIX: needed for std::sort
#include <cstdlib>

#pragma comment(lib, "shlwapi.lib") // ignored by MinGW but harmless

using namespace Gdiplus;

// -------------------- Utilities --------------------

static std::string CurrentTimestamp()
{
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_s(&tmv, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

static void AppendLog(const std::string &s)
{
    std::ofstream f("runtime.log", std::ios::app);
    if (f.is_open()) {
        f << CurrentTimestamp() << " - " << s << "\n";
    }
}

// Convert wide string (Windows) -> UTF-8 std::string
static std::string WStringToUtf8(const std::wstring &w)
{
    if (w.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return {};
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

// Convert UTF-8 std::string -> std::wstring (Windows)
static std::wstring Utf8ToWString(const std::string &s)
{
    if (s.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (size_needed <= 0) return {};
    std::wstring w(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], size_needed);
    return w;
}

static bool FileExistsW(const std::wstring &path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

// safe open text file with UTF-8 converter when input is wide path
static bool ReadFileAsUtf8(const std::wstring &wpath, std::string &out)
{
    std::string pathUtf8 = WStringToUtf8(wpath);
    std::ifstream f(pathUtf8, std::ios::in | std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    f.close();
    return true;
}

// -------------------- Scene / asset types --------------------

struct Asset {
    std::wstring name;      // filename (wide)
    std::wstring fullPath;  // full wide path to file on disk
    ULONG width = 0;
    ULONG height = 0;
    Bitmap *bmp = nullptr; // Gdiplus Bitmap pointer (owned)
};

struct Entity {
    std::string id;       // ascii id from scene
    std::wstring asset;   // wide filename matched to Asset.name
    int x = 0, y = 0, z = 0;
    Asset *assetPtr = nullptr;
};

// -------------------- Global state --------------------

static std::wstring g_exeFolder;       // wide
static std::wstring g_projectPath;     // wide (from config)
static std::vector<Asset> g_assets;
static std::vector<Entity> g_entities;
static ULONG g_windowW = 1280, g_windowH = 720;
static HWND g_hwnd = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static bool g_gdiInited = false;

// -------------------- Helpers: paths & scanning --------------------

static std::wstring GetExeFolderW()
{
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return L".";
    std::wstring p(buf, buf + len);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return p;
}

static bool ReadConfigAndResolveProject()
{
    std::wstring cfg = g_exeFolder + L"\\config.txt";
    std::string content;
    if (!ReadFileAsUtf8(cfg, content)) {
        AppendLog("ReadConfig: config.txt not found at: " + WStringToUtf8(cfg));
        return false;
    }
    AppendLog("Read config content (first 1024 bytes): " + content.substr(0, std::min<size_t>(content.size(), 1024)));

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.find("project_path=") == 0) {
            std::string val = line.substr(strlen("project_path="));
            g_projectPath = Utf8ToWString(val);
            AppendLog("Resolved project path (from config): " + WStringToUtf8(g_projectPath));
            return true;
        }
    }
    AppendLog("ReadConfig: project_path not found in config.txt");
    return false;
}

static void ScanAssetsFolder()
{
    g_assets.clear();
    std::wstring assetsDir = g_projectPath + L"\\assets";
    AppendLog("Scanning assets in: " + WStringToUtf8(assetsDir));

    std::wstring search = assetsDir + L"\\*.*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        AppendLog("No assets folder or empty: " + WStringToUtf8(assetsDir));
        return;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Asset a;
        a.name = name;
        a.fullPath = assetsDir + L"\\" + name;
        g_assets.push_back(a);
        AppendLog("Found asset: " + WStringToUtf8(name));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static Asset* FindAssetByName(const std::wstring &name)
{
    for (auto &a : g_assets) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

// -------------------- Scene parser (naive, tuned to your format) --------------------

static std::string ExtractStringValue(const std::string &s, size_t &i)
{
    while (i < s.size() && s[i] != '"') ++i;
    if (i >= s.size() || s[i] != '"') return {};
    ++i;
    std::string out;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') break;
        if (c == '\\' && i < s.size()) {
            char n = s[i++];
            out.push_back(n);
        } else out.push_back(c);
    }
    return out;
}

static int ExtractIntValue(const std::string &s, size_t &i)
{
    while (i < s.size() && !((s[i] >= '0' && s[i] <= '9') || s[i] == '-')) ++i;
    if (i >= s.size()) return 0;
    int sign = 1;
    if (s[i] == '-') { sign = -1; ++i; }
    long long val = 0;
    while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) {
        val = val * 10 + (s[i] - '0');
        ++i;
    }
    return (int)(val * sign);
}

static void LoadSceneEntities()
{
    g_entities.clear();
    std::wstring scenePathW = g_projectPath + L"\\scenes\\main.scene";
    AppendLog("Loading scene from: " + WStringToUtf8(scenePathW));
    std::string content;
    if (!ReadFileAsUtf8(scenePathW, content)) {
        AppendLog("ERROR: could not open scene file: " + WStringToUtf8(scenePathW));
        return;
    }
    AppendLog("Scene file content (first 1024 bytes):\n" + content.substr(0, std::min<size_t>(content.size(), 1024)));

    size_t pos = 0;
    while (true) {
        size_t objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = content.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string block = content.substr(objStart, objEnd - objStart + 1);

        if (block.find("\"asset\"") != std::string::npos) {
            Entity e;
            size_t idx = 0;
            size_t pid = block.find("\"id\"", idx);
            if (pid != std::string::npos) {
                idx = pid;
                std::string key = ExtractStringValue(block, idx);
                size_t q = block.find('"', idx);
                if (q != std::string::npos) {
                    idx = q;
                    e.id = ExtractStringValue(block, idx);
                }
            }
            size_t passet = block.find("\"asset\"", 0);
            if (passet != std::string::npos) {
                size_t idx2 = passet;
                std::string k = ExtractStringValue(block, idx2);
                size_t q = block.find('"', idx2);
                if (q != std::string::npos) { idx2 = q; std::string assetName = ExtractStringValue(block, idx2);
                    e.asset = Utf8ToWString(assetName);
                }
            }
            size_t px = block.find("\"x\"", 0);
            if (px != std::string::npos) {
                size_t i = px;
                size_t colon = block.find(':', i);
                if (colon != std::string::npos) { i = colon+1; e.x = ExtractIntValue(block, i); }
            }
            size_t py = block.find("\"y\"", 0);
            if (py != std::string::npos) {
                size_t i = py;
                size_t colon = block.find(':', i);
                if (colon != std::string::npos) { i = colon+1; e.y = ExtractIntValue(block, i); }
            }
            size_t pz = block.find("\"z\"", 0);
            if (pz != std::string::npos) {
                size_t i = pz;
                size_t colon = block.find(':', i);
                if (colon != std::string::npos) { i = colon+1; e.z = ExtractIntValue(block, i); }
            }
            g_entities.push_back(e);
        }
        pos = objEnd + 1;
    }

    AppendLog("Loaded entities count: " + std::to_string(g_entities.size()));
    for (auto &ent : g_entities) {
        Asset *a = FindAssetByName(ent.asset);
        if (a) ent.assetPtr = a;
        AppendLog("Entity: id=" + ent.id + ", asset=" + WStringToUtf8(ent.asset) + ", pos=" + std::to_string(ent.x) + "," + std::to_string(ent.y));
    }
}

// -------------------- Load bitmaps --------------------

static void LoadBitmaps()
{
    for (auto &a : g_assets) {
        std::wstring full = a.fullPath;
        if (!FileExistsW(full)) {
            AppendLog("Asset file missing: " + WStringToUtf8(full));
            continue;
        }
        Bitmap *bmp = Bitmap::FromFile(full.c_str(), false);
        if (!bmp) {
            AppendLog("GDI+: failed to create bitmap for: " + WStringToUtf8(full));
            continue;
        }
        Status st = bmp->GetLastStatus();
        if (st != Ok) {
            AppendLog("GDI+: bitmap status not OK for: " + WStringToUtf8(full));
            delete bmp;
            continue;
        }
        a.bmp = bmp;
        a.width = bmp->GetWidth();
        a.height = bmp->GetHeight();
        AppendLog("Loaded asset: " + WStringToUtf8(a.name) + " (" + std::to_string(a.width) + "x" + std::to_string(a.height) + ")");
    }
}

// -------------------- Win32 drawing --------------------

static void PaintScene(HDC hdc)
{
    Graphics g(hdc);
    SolidBrush bgBrush(Color(255, 108, 139, 182));
    g.FillRectangle(&bgBrush, 0, 0, (INT)g_windowW, (INT)g_windowH);

    Pen gridPen(Color(80, 40, 40, 40));
    int cell = 40;
    for (int x = 0; x < (int)g_windowW; x += cell) g.DrawLine(&gridPen, REAL(x), 0.0f, REAL(x), (REAL)g_windowH);
    for (int y = 0; y < (int)g_windowH; y += cell) g.DrawLine(&gridPen, 0.0f, REAL(y), (REAL)g_windowW, REAL(y));

    std::vector<Entity*> order;
    for (auto &e : g_entities) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](Entity *a, Entity *b){ return a->z < b->z; });

    for (auto p : order) {
        if (p->assetPtr && p->assetPtr->bmp) {
            Bitmap *bmp = p->assetPtr->bmp;
            UINT bw = bmp->GetWidth();
            UINT bh = bmp->GetHeight();
            REAL destX = (REAL)p->x;
            REAL destY = (REAL)p->y;
            g.DrawImage(bmp, destX, destY, (REAL)bw, (REAL)bh);
        } else {
            SolidBrush red(Color(255, 200, 80, 80));
            FontFamily ff(L"Segoe UI");
            Font font(&ff, 18.0f, FontStyleRegular, UnitPixel);
            SolidBrush white(Color(255, 255, 255, 255));
            g.FillRectangle(&red, (REAL)p->x, (REAL)p->y, 48.0f, 48.0f);
            PointF pt((REAL)p->x+10, (REAL)p->y+10);
            std::wstring wid = Utf8ToWString(p->id);
            g.DrawString(wid.c_str(), -1, &font, pt, &white);
        }
    }
}

// -------------------- WinProc --------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintScene(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        g_windowW = LOWORD(lParam);
        g_windowH = HIWORD(lParam);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// -------------------- Initialization and main --------------------

static bool InitGDIPlus()
{
    GdiplusStartupInput gdiplusStartupInput;
    Status st = GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
    if (st != Ok) {
        AppendLog("GdiplusStartup failed");
        return false;
    }
    g_gdiInited = true;
    AppendLog("Gdiplus initialized successfully");
    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    std::ofstream f("runtime.log", std::ios::trunc);
    f << CurrentTimestamp() << " - --- Thunderz Runtime (debug) startup ---\n";
    f.close();

    g_exeFolder = GetExeFolderW();
    AppendLog("EXE folder: " + WStringToUtf8(g_exeFolder));

    if (!ReadConfigAndResolveProject()) {
        AppendLog("FATAL: Could not resolve project path. Exiting.");
        MessageBoxW(NULL, L"Could not read config.txt or project_path missing. See runtime.log", L"Error", MB_ICONERROR);
        return 1;
    }

    ScanAssetsFolder();
    LoadSceneEntities();

    if (!InitGDIPlus()) {
        MessageBoxW(NULL, L"GDI+ init failed. See runtime.log", L"Error", MB_ICONERROR);
        return 1;
    }

    LoadBitmaps();

    const wchar_t CLASS_NAME[] = L"ThunderzRuntimeWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    RegisterClassW(&wc);

    int winW = 1280, winH = 720;
    g_windowW = winW; g_windowH = winH;
    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Thunderz Runtime",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        NULL,
        NULL,
        hInstance,
        NULL
    );
    if (!hwnd) {
        AppendLog("CreateWindowExW failed");
        return 1;
    }
    g_hwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (auto &a : g_assets) {
        if (a.bmp) {
            delete a.bmp;
            a.bmp = nullptr;
        }
    }

    if (g_gdiInited) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiInited = false;
    }

    AppendLog("Runtime exiting normally");
    return 0;
}
