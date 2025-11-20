// ThunderzEditor.cpp
// Fixed version — use as drop-in replacement.
// Build: g++ -std=c++17 -municode -lgdiplus -lshlwapi ThunderzEditor.cpp -o ThunderzEditor.exe

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM macros
#include <gdiplus.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>

#pragma comment (lib,"Gdiplus.lib")
#pragma comment (lib,"Shlwapi.lib")

namespace fs = std::filesystem;
using std::wstring;
using std::string;

static const char* RUNTIME_EXE_NAME = "ThunderzRuntime.exe";
static const wchar_t* WINDOW_CLASS = L"ThunderzEditorClass";
static const wchar_t* WINDOW_TITLE = L"Thunderz Editor";

static std::mutex g_mutex;

// ---------- Logging (editor.log) ----------
static std::wstring g_exeFolder;
static std::wstring g_configPath;
static std::wstring g_projectPath;
static std::wstring g_logPath;

static void AppendLog(const std::wstring &s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::wofstream f(g_logPath.c_str(), std::ios::app);
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t buf[300];
        swprintf(buf, 300, L"%04d-%02d-%02d %02d:%02d:%02d - %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, s.c_str());
        f << buf;
    }
}

// ---------- Simple data structures ----------
struct Asset {
    std::string name; // filename
    int w=0,h=0;
    bool loaded=false;
};

struct Entity {
    std::string id;
    std::string asset; // filename
    int x=0, y=0, z=0;
};

static std::vector<Asset> g_assets;
static std::vector<Entity> g_entities;
static std::string g_sceneId = "main";

// ---------- UI state ----------
static bool g_needsRepaint = true;
static Gdiplus::Bitmap* g_backBuffer = nullptr;
static int g_backW=0, g_backH=0;

static std::atomic<bool> g_isPlaying(false);
static PROCESS_INFORMATION g_runtimeProc = {};
static bool g_saveKeyDown = false;

static HWND g_hWnd = NULL;
static int g_mouseDown = 0;
static int g_dragEntityIndex = -1;
static POINT g_lastMousePt = {0,0};

// ---------- Forward declarations ----------
static void EnsureBackBuffer(int w, int h);
static void PaintEditor(HWND hwnd);
static void DrawEditorContents(Gdiplus::Graphics &g, int w, int h);
static void ScanProjectAssets();
static bool LoadSceneFile();
static bool SaveSceneFile();
static void LaunchRuntimeIfNotRunning(HWND hwnd);

// ---------- Utilities ----------
static std::string WToA(const std::wstring &ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8,0,ws.c_str(),-1,&s[0],n,NULL,NULL);
    if(!s.empty() && s.back()==0) s.pop_back();
    return s;
}
static std::wstring AToW(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,NULL,0);
    std::wstring ws(n,0);
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&ws[0],n);
    if(!ws.empty() && ws.back()==0) ws.pop_back();
    return ws;
}

static std::string ReadFileToString(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if(!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------- VERY simple JSON parser for our small scene format ----------
static bool parse_scene_simple(const std::string &json, std::string &out_id, std::vector<Entity> &out_entities) {
    out_entities.clear();
    out_id.clear();
    size_t i=0, n=json.size();
    auto skip = [&](void){ while(i<n && isspace((unsigned char)json[i])) ++i; };
    skip();
    size_t idpos = json.find("\"id\"", i);
    if (idpos==std::string::npos) return false;
    size_t colon = json.find(':', idpos);
    if (colon==std::string::npos) return false;
    size_t q1 = json.find('"', colon);
    if (q1==std::string::npos) return false;
    size_t q2 = json.find('"', q1+1);
    if (q2==std::string::npos) return false;
    out_id = json.substr(q1+1, q2-q1-1);

    size_t entitiesPos = json.find("\"entities\"", q2);
    if (entitiesPos==std::string::npos) return true;
    size_t arrOpen = json.find('[', entitiesPos);
    if (arrOpen==std::string::npos) return false;
    size_t arrClose = json.find(']', arrOpen);
    if (arrClose==std::string::npos) return false;
    size_t p = arrOpen+1;
    while (p < arrClose) {
        size_t objOpen = json.find('{', p);
        if (objOpen==std::string::npos || objOpen>=arrClose) break;
        size_t objClose = json.find('}', objOpen);
        if (objClose==std::string::npos || objClose>arrClose) break;
        std::string obj = json.substr(objOpen+1, objClose-objOpen-1);
        Entity e;
        auto get_str = [&](const char* key)->std::string {
            size_t kp = obj.find(std::string("\"") + key + "\"");
            if (kp==std::string::npos) return {};
            size_t c = obj.find(':', kp);
            if (c==std::string::npos) return {};
            size_t q1 = obj.find('"', c);
            if (q1==std::string::npos) return {};
            size_t q2 = obj.find('"', q1+1);
            if (q2==std::string::npos) return {};
            return obj.substr(q1+1, q2-q1-1);
        };
        auto get_int = [&](const char* key, int def)->int {
            size_t kp = obj.find(std::string("\"") + key + "\"");
            if (kp==std::string::npos) return def;
            size_t c = obj.find(':', kp);
            if (c==std::string::npos) return def;
            size_t start = c+1;
            while (start<obj.size() && isspace((unsigned char)obj[start])) ++start;
            int sign = 1;
            if (obj[start]=='-'){ sign=-1; ++start; }
            int val=0;
            bool found=false;
            while (start<obj.size() && isdigit((unsigned char)obj[start])) { found=true; val=val*10 + (obj[start]-'0'); ++start; }
            if (!found) return def;
            return val*sign;
        };
        e.id = get_str("id");
        e.asset = get_str("asset");
        e.x = get_int("x",0);
        e.y = get_int("y",0);
        e.z = get_int("z",0);
        out_entities.push_back(e);
        p = objClose+1;
    }
    return true;
}

static std::string write_scene_simple(const std::string &id, const std::vector<Entity> &entities) {
    std::ostringstream ss;
    ss << "{\n  \"id\": \"" << id << "\",\n  \"entities\": [\n";
    for (size_t i=0;i<entities.size();++i) {
        const Entity &e = entities[i];
        ss << "    { \"id\":\"" << e.id << "\", \"asset\":\"" << e.asset << "\", \"x\":" << e.x << ", \"y\":" << e.y << ", \"z\":" << e.z << " }";
        if (i+1<entities.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ],\n  \"script\": []\n}\n";
    return ss.str();
}

// ---------- Asset loading ----------
static void ScanProjectAssets() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_assets.clear();
    if (g_projectPath.empty()) return;
    fs::path assetsFolder = fs::path(g_projectPath) / "assets";
    AppendLog(std::wstring(L"Scanning assets in: ") + assetsFolder.wstring());
    if (!fs::exists(assetsFolder)) return;
    for (auto &p : fs::directory_iterator(assetsFolder)) {
        if (!p.is_regular_file()) continue;
        Asset a;
        a.name = p.path().filename().string();
        g_assets.push_back(a);
        AppendLog(std::wstring(L"Found asset: ") + p.path().filename().wstring());
    }
}

// ---------- Scene load/save ----------
static bool LoadSceneFile() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_projectPath.empty()) return false;
    fs::path scenePath = fs::path(g_projectPath) / "scenes" / "main.scene";
    AppendLog(std::wstring(L"Loading scene from: ") + scenePath.wstring());
    if (!fs::exists(scenePath)) {
        AppendLog(std::wstring(L"Scene missing: ") + scenePath.wstring());
        return false;
    }
    std::string content = ReadFileToString(scenePath.string());
    if (content.empty()) {
        AppendLog(L"Scene empty or unreadable");
        return false;
    }
    std::vector<Entity> ent;
    std::string id;
    if (!parse_scene_simple(content, id, ent)) {
        AppendLog(L"Scene parse failed");
        return false;
    }
    g_entities = ent;
    g_sceneId = id.empty() ? "main" : id;
    AppendLog(std::wstring(L"Loaded entities count: ") + std::to_wstring(g_entities.size()));
    return true;
}

static bool SaveSceneFile() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_projectPath.empty()) return false;
    fs::path sceneDir = fs::path(g_projectPath) / "scenes";
    fs::create_directories(sceneDir);
    fs::path scenePath = sceneDir / "main.scene";
    std::string txt = write_scene_simple(g_sceneId, g_entities);
    std::ofstream f(scenePath.string(), std::ios::binary);
    if (!f) {
        AppendLog(L"Save scene failed: cannot open file");
        return false;
    }
    f << txt;
    AppendLog(L"Saved scene: OK");
    return true;
}

// ---------- GDI+ back buffer ----------
static void EnsureBackBuffer(int w, int h) {
    if (w<=0 || h<=0) return;
    if (!g_backBuffer || g_backW!=w || g_backH!=h) {
        if (g_backBuffer) delete g_backBuffer;
        g_backBuffer = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
        g_backW = w; g_backH = h;
    }
}

// ---------- Draw helpers ----------
static Gdiplus::Color color_from_rgb(int r,int g,int b){ return Gdiplus::Color(255,r,g,b); }

static void DrawEntityPreview(Gdiplus::Graphics &g, const Entity &e) {
    Gdiplus::SolidBrush brush(color_from_rgb(210,100,100));
    Gdiplus::Pen pen(color_from_rgb(160,60,60), 2.0f);
    float w=48.0f, h=48.0f;
    g.FillRectangle(&brush, (Gdiplus::REAL)e.x, (Gdiplus::REAL)e.y, w, h);
    g.DrawRectangle(&pen, (Gdiplus::REAL)e.x, (Gdiplus::REAL)e.y, w, h);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tb(color_from_rgb(255,255,255));
    Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF rect((Gdiplus::REAL)e.x, (Gdiplus::REAL)e.y+8.0f, w, 32.0f);
    std::wstring idw = AToW(e.id);
    g.DrawString(idw.c_str(), -1, &font, rect, &sf, &tb);
}

static void DrawGrid(Gdiplus::Graphics &g, int w, int h) {
    const int step = 32;
    Gdiplus::Pen pen(color_from_rgb(60,60,70), 1.0f);
    pen.SetDashStyle(Gdiplus::DashStyleSolid);
    for (int x=0;x<w;x+=step) g.DrawLine(&pen, (Gdiplus::REAL)x, 0.0f, (Gdiplus::REAL)x, (Gdiplus::REAL)h);
    for (int y=0;y<h;y+=step) g.DrawLine(&pen, 0.0f, (Gdiplus::REAL)y, (Gdiplus::REAL)w, (Gdiplus::REAL)y);
}

static void DrawInspector(Gdiplus::Graphics &g, int left, int top, int w, int h) {
    Gdiplus::SolidBrush tbrush(color_from_rgb(220,220,220));
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 11, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    int y = top + 10;
    g.DrawString(L"Inspector", -1, &font, Gdiplus::PointF((Gdiplus::REAL)left,(Gdiplus::REAL)y), &tbrush);
    y += 30;

    int sel = -1;
    if (!g_entities.empty()) sel=0;
    if (sel>=0 && sel < (int)g_entities.size()) {
        Entity &e = g_entities[sel];
        std::wstring s = AToW(e.id);
        g.DrawString(s.c_str(), -1, &font, Gdiplus::PointF((Gdiplus::REAL)left, (Gdiplus::REAL)y), &tbrush);
        y += 24;
        Gdiplus::Font fsmall(&ff, 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        g.DrawString(L"X:", -1, &fsmall, Gdiplus::PointF((Gdiplus::REAL)left, (Gdiplus::REAL)y), &tbrush);
        g.DrawString(L"Y:", -1, &fsmall, Gdiplus::PointF((Gdiplus::REAL)left+80.0f, (Gdiplus::REAL)y), &tbrush);
        g.DrawString(L"Z:", -1, &fsmall, Gdiplus::PointF((Gdiplus::REAL)left+160.0f, (Gdiplus::REAL)y), &tbrush);
        wchar_t buf[64];
        swprintf(buf, 64, L"%d", e.x);
        g.DrawString(buf, -1, &fsmall, Gdiplus::PointF((Gdiplus::REAL)left+24.0f, (Gdiplus::REAL)y), &tbrush);
        swprintf(buf, 64, L"%d", e.y);
        g.DrawString(buf, -1, &fsmall, Gdiplus::PointF((Gdiplus::REAL)left+104.0f, (Gdiplus::REAL)y), &tbrush);
        swprintf(buf, 64, L"%d", e.z);
        g.DrawString(buf, -1, &fsmall, Gdiplus::PointF((Gdiplus::REAL)left+184.0f, (Gdiplus::REAL)y), &tbrush);
    }
}

static void DrawEditorContents(Gdiplus::Graphics &g, int w, int h) {
    g.Clear(color_from_rgb(38,40,44));
    const int leftW = 200;
    const int rightW = 260;
    const int bottomH = 120;
    int sceneW = w - leftW - rightW;
    int sceneH = h - bottomH;

    Gdiplus::SolidBrush panel(color_from_rgb(30,32,36));
    g.FillRectangle(&panel, 0, 0, leftW, h);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush text(color_from_rgb(200,200,200));
    g.DrawString(L"Hierarchy", -1, &font, Gdiplus::PointF(10,8), &text);

    g.FillRectangle(&panel, leftW, h-bottomH, sceneW, bottomH);
    g.DrawString(L"Assets", -1, &font, Gdiplus::PointF(10,h-bottomH+6), &text);

    g.FillRectangle(&panel, leftW+sceneW, 0, rightW, h);
    DrawInspector(g, leftW+sceneW+8, 8, rightW-16, h-16);

    Gdiplus::SolidBrush sceneBg(color_from_rgb(60,64,73));
    g.FillRectangle(&sceneBg, leftW, 0, sceneW, sceneH);
    DrawGrid(g, sceneW, sceneH);

    for (size_t i=0;i<g_entities.size();++i) {
        DrawEntityPreview(g, g_entities[i]);
    }

    int thumbX = leftW+8;
    int thumbY = h-bottomH+36;
    for (size_t i=0;i<g_assets.size();++i) {
        Gdiplus::SolidBrush tb(color_from_rgb(80,150,210));
        g.FillRectangle(&tb, (Gdiplus::REAL)thumbX, (Gdiplus::REAL)thumbY, 56.0f, 56.0f);
        Gdiplus::Pen p(color_from_rgb(60,110,170), 2.0f);
        g.DrawRectangle(&p, (Gdiplus::REAL)thumbX, (Gdiplus::REAL)thumbY, 56.0f, 56.0f);
        std::wstring nm = AToW(g_assets[i].name);
        Gdiplus::Font fsmall(&ff, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(nm.c_str(), -1, &fsmall, Gdiplus::RectF((Gdiplus::REAL)thumbX, (Gdiplus::REAL)thumbY+60.0f, 56.0f, 16.0f), &sf, &text);
        thumbX += 72;
    }

    Gdiplus::SolidBrush bar(color_from_rgb(50,50,55));
    g.FillRectangle(&bar, 0, 0, w, 40);
    Gdiplus::SolidBrush green(color_from_rgb(45,145,80));
    Gdiplus::SolidBrush blue(color_from_rgb(60,100,180));
    g.FillRectangle(&green, 12, 6, 80, 28);
    g.FillRectangle(&blue, 102, 6, 80, 28);

    Gdiplus::Font fbtn(&ff, 12, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    g.DrawString(L"Save (S)", -1, &fbtn, Gdiplus::PointF(18,10), &text);
    g.DrawString(L"Play (P)", -1, &fbtn, Gdiplus::PointF(112,10), &text);

    std::wstring status = g_isPlaying ? L"Runtime: running" : L"Runtime: stopped";
    g.DrawString(status.c_str(), -1, &font, Gdiplus::PointF((Gdiplus::REAL)w-180.0f,10.0f), &text);
}

// ---------- Painting (double buffer) ----------
static void PaintEditor(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w<=0 || h<=0) return;
    EnsureBackBuffer(w,h);
    {
        Gdiplus::Graphics g(g_backBuffer);
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        DrawEditorContents(g, w, h);
    }
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        Gdiplus::Graphics g2(hdc);
        g2.DrawImage(g_backBuffer, 0, 0, w, h);
        ReleaseDC(hwnd, hdc);
    }
}

// ---------- Runtime launch ----------
static void LaunchRuntimeIfNotRunning(HWND hwnd) {
    if (g_isPlaying.load()) return;
    fs::path exe = fs::path(g_exeFolder) / RUNTIME_EXE_NAME;
    std::wstring exeW = exe.wstring();
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(exeW.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (!ok) {
        AppendLog(std::wstring(L"Failed to launch runtime: ") + exeW);
        return;
    }
    g_runtimeProc = pi;
    g_isPlaying.store(true);
    AppendLog(std::wstring(L"Launched runtime: ") + exeW);
    std::thread([hwnd]() {
        WaitForSingleObject(g_runtimeProc.hProcess, INFINITE);
        CloseHandle(g_runtimeProc.hProcess);
        CloseHandle(g_runtimeProc.hThread);
        g_isPlaying.store(false);
        g_needsRepaint = true;
        PostMessage(hwnd, WM_USER+200, 0, 0);
    }).detach();
}

// ---------- Window procedure ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hWnd = hwnd;
        AppendLog(L"--- Thunderz Editor (debug) startup ---");
        AppendLog(std::wstring(L"EXE folder: ") + g_exeFolder);
        AppendLog(std::wstring(L"Read config: project_path=") + g_projectPath);
        ScanProjectAssets();
        LoadSceneFile();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SIZE:
        g_needsRepaint = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        PaintEditor(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        g_mouseDown = 1;
        g_lastMousePt.x = mx; g_lastMousePt.y = my;
        int picked=-1;
        for (int i=(int)g_entities.size()-1;i>=0;--i) {
            Entity &e = g_entities[i];
            if (mx >= e.x && mx <= e.x+48 && my >= e.y && my <= e.y+48) { picked=i; break; }
        }
        g_dragEntityIndex = picked;
        g_needsRepaint = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        if (g_mouseDown && g_dragEntityIndex!=-1) {
            Entity &e = g_entities[g_dragEntityIndex];
            int dx = mx - g_lastMousePt.x;
            int dy = my - g_lastMousePt.y;
            e.x += dx; e.y += dy;
            g_lastMousePt.x = mx; g_lastMousePt.y = my;
            g_needsRepaint = true;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        g_mouseDown = 0;
        g_dragEntityIndex = -1;
        SaveSceneFile();
        g_needsRepaint = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (wParam == 'S' || wParam=='s')) {
            if (!g_saveKeyDown) {
                g_saveKeyDown = true;
                SaveSceneFile();
                g_needsRepaint = true;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        if ((wParam == 'P' || wParam=='p') || (ctrl && (wParam=='P' || wParam=='p'))) {
            LaunchRuntimeIfNotRunning(hwnd);
            g_needsRepaint = true;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_KEYUP:
        if (wParam == 'S' || wParam=='s') g_saveKeyDown = false;
        return 0;

    case WM_USER+200:
        AppendLog(L"Runtime exited");
        g_needsRepaint = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------- Startup: read config and init ----------
static bool ReadConfig() {
    fs::path cfg = fs::path(g_exeFolder) / "config.txt";
    g_configPath = cfg.wstring();
    std::wifstream fi(cfg.wstring().c_str());
    if (!fi) {
        AppendLog(std::wstring(L"Config not found: ") + cfg.wstring());
        return false;
    }
    std::wstring line;
    while (std::getline(fi, line)) {
        auto trim = [&](std::wstring &s){ while(!s.empty() && iswspace(s.back())) s.pop_back(); while(!s.empty() && iswspace(s.front())) s.erase(s.begin()); };
        trim(line);
        if (line.rfind(L"project_path=",0)==0) {
            std::wstring v = line.substr(13);
            trim(v);
            g_projectPath = v;
        }
    }
    fi.close();
    AppendLog(std::wstring(L"Read config: project_path=") + g_projectPath);
    return true;
}

// ---------- WinMain ----------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    fs::path p(exePath);
    g_exeFolder = p.parent_path().wstring();
    g_logPath = (fs::path(g_exeFolder) / "editor.log").wstring();
    {
        std::wofstream lf(g_logPath.c_str(), std::ios::trunc);
        lf << L"";
    }
    AppendLog(L"--- Thunderz Editor (debug) startup ---");
    AppendLog(std::wstring(L"EXE folder: ") + g_exeFolder);

    ReadConfig();

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::Status st = Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    if (st != Gdiplus::Ok) {
        AppendLog(L"Gdiplus init failed");
        return -1;
    }
    AppendLog(L"GDI+ initialized");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) { AppendLog(L"CreateWindow failed"); return -1; }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    ScanProjectAssets();
    LoadSceneFile();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_backBuffer) delete g_backBuffer;
    Gdiplus::GdiplusShutdown(gdiplusToken);
    AppendLog(L"Editor exiting");
    return 0;
}


