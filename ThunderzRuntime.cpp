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

#pragma comment(lib, "shlwapi.lib") // for PathFileExistsW if using MSVC; MinGW may ignore pragma

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
    // remove exe filename -> folder
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return p;
}

static bool ReadConfigAndResolveProject()
{
    // config path: EXE_FOLDER\config.txt
    std::wstring cfg = g_exeFolder + L"\\config.txt";
    std::string content;
    if (!ReadFileAsUtf8(cfg, content)) {
        AppendLog("ReadConfig: config.txt not found at: " + WStringToUtf8(cfg));
        return false;
    }
    AppendLog("Read config content (first 1024 bytes): " + content.substr(0, std::min<size_t>(content.size(), 1024)));

    // naive parse: find line starting with project_path=
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // trim spaces
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.find("project_path=") == 0) {
            std::string val = line.substr(strlen("project_path="));
            // convert to wstring. The config likely contains backslashes single backslash.
            // handle both forward and back slashes. We assume path in config is a normal Windows path.
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

    // Use Win32 FindFirstFileW to enumerate
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

// Find Asset by filename (case-sensitive)
static Asset* FindAssetByName(const std::wstring &name)
{
    for (auto &a : g_assets) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

// -------------------- Scene parser (naive, tuned to your format) --------------------
// Expects scene like:
// {
//  "id":"main",
//  "entities":[
//    { "id":"bg",   "asset":"bg.bmp",   "x":0,   "y":0,   "z":0 },
//    { "id":"hero", "asset":"hero.bmp", "x":300, "y":350, "z":1 }
//  ],
//  "script":[]
// }

static std::string SkipSpacesAnd(const std::string &s, size_t &i)
{
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return "";
}

static std::string ExtractStringValue(const std::string &s, size_t &i)
{
    // i should point to opening quote or whitespace before it
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
    // find next digit or '-' sign
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

    // naive search for entity objects
    size_t pos = 0;
    while (true) {
        // find next '{' that starts an entity object inside entities array
        size_t objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = content.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string block = content.substr(objStart, objEnd - objStart + 1);

        // check if this block contains "asset"
        if (block.find("\"asset\"") != std::string::npos) {
            Entity e;
            // parse id
            size_t idx = 0;
            // find "id":
            size_t pid = block.find("\"id\"", idx);
            if (pid != std::string::npos) {
                idx = pid;
                std::string key = ExtractStringValue(block, idx); // will find the first quote - not ideal for keys, but tuned
                // After extracting "id" key it leaves idx at next char after closing quote. We need to find next quoted value.
                // Find next quote for value:
                size_t q = block.find('"', idx);
                if (q != std::string::npos) {
                    idx = q;
                    e.id = ExtractStringValue(block, idx);
                }
            }
            // parse asset
            size_t passet = block.find("\"asset\"", 0);
            if (passet != std::string::npos) {
                size_t idx2 = passet;
                std::string k = ExtractStringValue(block, idx2); // gets "asset" key unexpectedly but we then find next quoted value
                size_t q = block.find('"', idx2);
                if (q != std::string::npos) { idx2 = q; std::string assetName = ExtractStringValue(block, idx2);
                    e.asset = Utf8ToWString(assetName);
                }
            }
            // parse x,y,z
            size_t px = block.find("\"x\"", 0);
            if (px != std::string::npos) {
                size_t i = px;
                // find colon
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

            // push entity
            g_entities.push_back(e);
        }

        pos = objEnd + 1;
    }

    AppendLog("Loaded entities count: " + std::to_string(g_entities.size()));
    // now attach asset pointers if available
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
        // use Gdiplus::Bitmap::FromFile which accepts wide LPCWSTR. Use a.fullPath.
        std::wstring full = a.fullPath;
        // If file doesn't exist or not BMP, skip
        if (!FileExistsW(full)) {
            AppendLog("Asset file missing: " + WStringToUtf8(full));
            continue;
        }
        // Load via GDI+
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
    // background fill - light blue
    SolidBrush bgBrush(Color(255, 108, 139, 182)); // muted blue
    g.FillRectangle(&bgBrush, 0, 0, (INT)g_windowW, (INT)g_windowH);

    // Draw grid in scene area (left portion). We'll draw full window grid for simplicity
    Pen gridPen(Color(80, 40, 40, 40));
    int cell = 40;
    for (int x = 0; x < (int)g_windowW; x += cell) g.DrawLine(&gridPen, REAL(x), 0.0f, REAL(x), (REAL)g_windowH);
    for (int y = 0; y < (int)g_windowH; y += cell) g.DrawLine(&gridPen, 0.0f, REAL(y), (REAL)g_windowW, REAL(y));

    // draw entities in order by z (lower z first)
    // simple sort copy
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
            // draw the bitmap at pos
            g.DrawImage(bmp, destX, destY, (REAL)bw, (REAL)bh);
        } else {
            // draw a placeholder box with the entity id
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
    // initialize empty log
    {
        std::ofstream f("runtime.log", std::ios::trunc);
        f << CurrentTimestamp() << " - --- Thunderz Runtime (debug) startup ---\n";
    }

    g_exeFolder = GetExeFolderW();
    AppendLog("EXE folder: " + WStringToUtf8(g_exeFolder));

    // Read config
    if (!ReadConfigAndResolveProject()) {
        AppendLog("FATAL: Could not resolve project path. Exiting.");
        MessageBoxW(NULL, L"Could not read config.txt or project_path missing. See runtime.log", L"Error", MB_ICONERROR);
        return 1;
    }

    // scan assets and load scene
    ScanAssetsFolder();
    LoadSceneEntities();

    // init GDI+
    if (!InitGDIPlus()) {
        MessageBoxW(NULL, L"GDI+ init failed. See runtime.log", L"Error", MB_ICONERROR);
        return 1;
    }

    // load bitmaps now that GDI+ is init
    LoadBitmaps();

    // register window class
    const wchar_t CLASS_NAME[] = L"ThunderzRuntimeWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    RegisterClassW(&wc);

    // create window
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

    // main message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // cleanup GDI+
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

