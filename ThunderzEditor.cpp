// ThunderzEditor.cpp
// Thunderz Editor - Phase1 UI (Custom modern style) - FIXED types
// Build with MinGW + GDI+:
// g++ -static -O2 -std=gnu++17 ThunderzEditor.cpp -o build/ThunderzEditor.exe -lgdiplus -lgdi32 -luser32 -lkernel32 -municode -mwindows

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <locale>
#include <codecvt>
#include <ctime>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ----------------- Utilities -----------------

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
    std::ofstream f("editor.log", std::ios::app);
    if (f.is_open()) {
        f << CurrentTimestamp() << " - " << s << "\n";
    }
}

static std::wstring Utf8ToW(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w; w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WToUtf8(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s; s.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

// ----------------- Data -----------------

struct Asset {
    std::wstring name;
    std::wstring fullPath;
    Bitmap *bmp = nullptr;
    UINT w=0,h=0;
};

struct Entity {
    std::string id;
    std::wstring asset; // asset filename (wide)
    int x=0,y=0,z=0;
    Asset* assetPtr = nullptr;
};

static std::wstring exeFolder;
static std::wstring projectPath;
static std::vector<Asset> assets;
static std::vector<Entity> entities;
static int selectedEntity = -1;

// UI layout values (will compute on resize)
static int winW = 1280, winH = 720;
static int toolbarH = 44;
static int leftW = 220;
static int rightW = 300;
static int assetsH = 180;
static HWND hwndGlobal = NULL;
static ULONG_PTR gdiToken = 0;
static bool gdiInited = false;

// ----------------- File helpers -----------------

static bool ReadFileUtf8(const std::wstring &path, std::string &out) {
    std::string p = WToUtf8(path);
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss; ss<<f.rdbuf();
    out = ss.str();
    return true;
}
static bool WriteFileUtf8(const std::wstring &path, const std::string &data) {
    std::string p = WToUtf8(path);
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(data.data(), data.size());
    return true;
}

static std::wstring GetExeFolder() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring p(buf, buf + n);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return p;
}

// ----------------- Load project & assets & scene -----------------

static void ScanAssetsFolder() {
    assets.clear();
    std::wstring ad = projectPath + L"\\assets";
    AppendLog("Scanning assets in: " + WToUtf8(ad));
    std::wstring search = ad + L"\\*.*";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        Asset a; a.name = name; a.fullPath = ad + L"\\" + name;
        assets.push_back(a);
        AppendLog("Found asset: " + WToUtf8(name));
    } while (FindNextFileW(h,&fd));
    FindClose(h);
}

static Asset* FindAsset(const std::wstring &name) {
    for (auto &a: assets) if (a.name==name) return &a;
    return nullptr;
}

static void LoadBitmaps() {
    for (auto &a: assets) {
        if (a.bmp) { delete a.bmp; a.bmp=nullptr;}
        Bitmap *b = Bitmap::FromFile(a.fullPath.c_str(), false);
        if (b) {
            if (b->GetLastStatus()==Ok) {
                a.bmp = b; a.w = b->GetWidth(); a.h = b->GetHeight();
                AppendLog("Loaded bitmap: " + WToUtf8(a.name));
            } else {
                delete b;
                AppendLog("Failed bitmap load (status): " + WToUtf8(a.name));
            }
        } else AppendLog("Bitmap null: " + WToUtf8(a.name));
    }
}

static void ParseScene() {
    entities.clear();
    std::wstring scenePath = projectPath + L"\\scenes\\main.scene";
    std::string content;
    if (!ReadFileUtf8(scenePath, content)) { AppendLog("main.scene missing or unreadable"); return; }
    AppendLog("Scene loaded: " + content.substr(0, std::min<size_t>(content.size(),1024)));
    // Minimal parser (tuned for your simple format)
    size_t pos=0;
    while (true) {
        size_t b = content.find('{', pos);
        if (b==std::string::npos) break;
        size_t e = content.find('}', b);
        if (e==std::string::npos) break;
        std::string block = content.substr(b, e-b+1);
        if (block.find("\"asset\"")!=std::string::npos) {
            Entity ent;
            // id
            size_t p = block.find("\"id\"");
            if (p!=std::string::npos) {
                size_t q = block.find('"', p+4);
                size_t r = block.find('"', q+1);
                size_t s = block.find('"', r+1);
                size_t t = block.find('"', s+1);
                if (s!=std::string::npos && t!=std::string::npos) {
                    std::string val = block.substr(s+1, t-s-1);
                    ent.id = val;
                }
            }
            // asset
            p = block.find("\"asset\"");
            if (p!=std::string::npos) {
                size_t q = block.find('"', p+6);
                size_t r = block.find('"', q+1);
                size_t s = block.find('"', r+1);
                size_t t = block.find('"', s+1);
                if (s!=std::string::npos && t!=std::string::npos) {
                    std::string val = block.substr(s+1, t-s-1);
                    ent.asset = Utf8ToW(val);
                }
            }
            // x,y,z
            auto getNum=[&](const std::string &blk, const char key)->int{
                size_t pp=blk.find(std::string("\"")+std::string(1,key)+std::string("\""));
                if (pp==std::string::npos) return 0;
                size_t colon = blk.find(':', pp);
                if (colon==std::string::npos) return 0;
                size_t comma = blk.find_first_of(",}", colon+1);
                std::string s = blk.substr(colon+1, comma-colon-1);
                try { return std::stoi(s); } catch(...) { return 0; }
            };
            ent.x = getNum(block,'x');
            ent.y = getNum(block,'y');
            ent.z = getNum(block,'z');
            entities.push_back(ent);
        }
        pos = e+1;
    }
    // attach asset pointers
    for (auto &en: entities) {
        Asset* a = FindAsset(en.asset);
        if (a) en.assetPtr = a;
    }
    AppendLog("Parsed entities: " + std::to_string(entities.size()));
}

static void SaveScene() {
    // simple serializer matching original format
    std::wstring scenePath = projectPath + L"\\scenes\\main.scene";
    std::ostringstream ss;
    ss << "{\n  \"id\":\"main\",\n  \"entities\":[\n";
    for (size_t i=0;i<entities.size();++i) {
        auto &e = entities[i];
        ss << "    { \"id\":\"" << e.id << "\", \"asset\":\"" << WToUtf8(e.asset) << "\", \"x\":" << e.x << ", \"y\":" << e.y << ", \"z\":" << e.z << " }";
        if (i+1<entities.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ],\n  \"script\":[]\n}\n";
    std::string out = ss.str();
    bool ok = WriteFileUtf8(scenePath, out);
    AppendLog(std::string("Saved scene: ") + (ok? "OK":"FAILED"));
}

// ----------------- UI helpers -----------------

static void DrawTextNice(Graphics &g, const std::wstring &text, REAL x, REAL y, REAL size=12.0f) {
    FontFamily ff(L"Segoe UI");
    Font font(&ff, size, FontStyleRegular, UnitPixel);
    SolidBrush col(Color(255,240,240,240));
    PointF pt(x,y);
    g.DrawString(text.c_str(), -1, &font, pt, &col);
}

// ----------------- Painting -----------------

static void PaintAll(HDC hdc) {
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // background
    LinearGradientBrush bgBrush(Rect(0,0,winW,winH), Color(255,25,25,30), Color(255,18,24,36), LinearGradientModeVertical);
    g.FillRectangle(&bgBrush, 0, 0, winW, winH);

    // toolbar
    RectF toolbarRect(0,0, (REAL)winW, (REAL)toolbarH);
    SolidBrush toolbarBg(Color(255,18,18,20));
    g.FillRectangle(&toolbarBg, toolbarRect);

    FontFamily ff(L"Segoe UI");
    Font fnt(&ff, 16.0f, FontStyleBold, UnitPixel);
    SolidBrush white(Color(255,230,230,235));
    PointF titlePt(12.0f, 12.0f);
    g.DrawString(L"Thunderz Engine - Editor", -1, &fnt, titlePt, &white);

    // toolbar buttons (Save, Play)
    RectF saveR(220,6,96,32), playR(324,6,96,32);
    SolidBrush saveBg(Color(255,40,120,70));
    SolidBrush playBg(Color(255,60,90,160));
    g.FillRectangle(&saveBg, saveR);
    g.FillRectangle(&playBg, playR);
    Font btnF(&ff,12.0f, FontStyleRegular, UnitPixel);
    SolidBrush btnTxt(Color(255,245,245,245));
    g.DrawString(L"Save (S)", -1, &btnF, PointF(saveR.X+18, saveR.Y+8), &btnTxt);
    g.DrawString(L"Play (P)", -1, &btnF, PointF(playR.X+20, playR.Y+8), &btnTxt);

    // left panel (Hierarchy)
    RectF leftRect(0, toolbarH, (REAL)leftW, (REAL)(winH - toolbarH));
    SolidBrush panelBg(Color(255,20,20,24));
    g.FillRectangle(&panelBg, leftRect);
    // header
    g.DrawString(L"Hierarchy", -1, &btnF, PointF(12, toolbarH + 8), &white);

    // right panel (Inspector)
    RectF rightRect((REAL)(winW-rightW), (REAL)toolbarH, (REAL)rightW, (REAL)(winH - toolbarH));
    SolidBrush rightBg(Color(255,18,18,22));
    g.FillRectangle(&rightBg, rightRect);
    g.DrawString(L"Inspector", -1, &btnF, PointF((REAL)(winW-rightW)+12, toolbarH+8), &white);

    // bottom-left assets area
    RectF assetsRect(0, (REAL)(winH-assetsH), (REAL)leftW, (REAL)assetsH);
    SolidBrush assetsBg(Color(255,14,14,18));
    g.FillRectangle(&assetsBg, assetsRect);
    g.DrawString(L"Assets", -1, &btnF, PointF(12, (REAL)(winH-assetsH)+8), &white);

    // scene view rect
    RectF sceneRect((REAL)leftW, (REAL)toolbarH, (REAL)(winW-leftW-rightW), (REAL)(winH-toolbarH-assetsH));
    SolidBrush sceneBg(Color(255,30,30,36));
    g.FillRectangle(&sceneBg, sceneRect);

    // draw entities in scene
    // sort by z
    std::vector<int> order;
    for (size_t i=0;i<entities.size();++i) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b){
        return entities[a].z < entities[b].z;
    });

    for (int idx: order) {
        Entity &en = entities[idx];
        if (en.assetPtr && en.assetPtr->bmp) {
            Bitmap *bmp = en.assetPtr->bmp;
            REAL dx = (REAL)leftW + (REAL)en.x;
            REAL dy = (REAL)toolbarH + (REAL)en.y;
            g.DrawImage(bmp, dx, dy, (REAL)en.assetPtr->w, (REAL)en.assetPtr->h);
        } else {
            // placeholder
            SolidBrush ph(Color(255,180,60,60));
            g.FillRectangle(&ph, (REAL)leftW + (REAL)en.x, (REAL)toolbarH + (REAL)en.y, (REAL)48, (REAL)48);
            Font small(&ff,10, FontStyleRegular, UnitPixel);
            g.DrawString(Utf8ToW(en.id).c_str(), -1, &small, PointF((REAL)leftW + (REAL)en.x + 2, (REAL)toolbarH + (REAL)en.y + 2), &white);
        }
    }

    // highlight selected entity with outline
    if (selectedEntity >=0 && selectedEntity < (int)entities.size()) {
        Entity &s = entities[selectedEntity];
        Pen selPen(Color(255,180,220,255), 2.0f);
        RectF rect((REAL)leftW + (REAL)s.x - 3.0f, (REAL)toolbarH + (REAL)s.y - 3.0f,
                       (s.assetPtr? (REAL)s.assetPtr->w + 6.0f : 54.0f),
                       (s.assetPtr? (REAL)s.assetPtr->h + 6.0f : 54.0f));
        g.DrawRectangle(&selPen, rect.X, rect.Y, rect.Width, rect.Height);
    }

    // draw hierarchy list
    Font hfont(&ff, 12.0f, FontStyleRegular, UnitPixel);
    int y = toolbarH + 36;
    for (size_t i=0;i<entities.size();++i) {
        Entity &e = entities[i];
        RectF item((REAL)8, (REAL)y, (REAL)leftW - 16, 24.0f);
        SolidBrush itemBg((i==selectedEntity)? Color(255,40,40,60) : Color(255,18,18,22));
        g.FillRectangle(&itemBg, item);
        g.DrawString(Utf8ToW(e.id).c_str(), -1, &hfont, PointF(12, (REAL)y+4), &white);
        y += 28;
    }

    // draw assets thumbnails (grid)
    int ax = 8, ay = winH - assetsH + 36;
    int thumb = 64;
    int gap = 8;
    int count=0;
    for (auto &a: assets) {
        if (a.bmp) {
            REAL px = (REAL)ax + (REAL)(count%2)*(thumb+gap);
            REAL py = (REAL)ay + (REAL)(count/2)*(thumb+gap);
            g.DrawImage(a.bmp, px, py, (REAL)thumb, (REAL)thumb);
            // name
            Font aname(&ff, 9.0f, FontStyleRegular, UnitPixel);
            g.DrawString(a.name.c_str(), -1, &aname, PointF(px, py+thumb+2), &white);
            count++;
        } else {
            // placeholder
            SolidBrush pb(Color(255,40,40,40));
            g.FillRectangle(&pb, (REAL)ax + (REAL)(count%2)*(thumb+gap), (REAL)ay + (REAL)(count/2)*(thumb+gap), (REAL)thumb, (REAL)thumb);
            count++;
        }
    }

    // inspector content (if selected)
    if (selectedEntity>=0 && selectedEntity < (int)entities.size()) {
        Entity &si = entities[selectedEntity];
        int insX = winW - rightW + 12;
        int insY = toolbarH + 36;
        Font lab(&ff, 11.0f, FontStyleRegular, UnitPixel);
        g.DrawString(L"ID:", -1, &lab, PointF((REAL)insX, (REAL)insY), &white);
        g.DrawString(Utf8ToW(si.id).c_str(), -1, &lab, PointF((REAL)insX+40, (REAL)insY), &white);
        insY += 28;
        g.DrawString(L"Asset:", -1, &lab, PointF((REAL)insX, (REAL)insY), &white);
        std::wstring an = si.asset.empty()? L"(none)": si.asset;
        g.DrawString(an.c_str(), -1, &lab, PointF((REAL)insX+60, (REAL)insY), &white);
        insY += 28;
        g.DrawString(L"X:", -1, &lab, PointF((REAL)insX, (REAL)insY), &white);
        g.DrawString(std::to_wstring(si.x).c_str(), -1, &lab, PointF((REAL)insX+20, (REAL)insY), &white);
        insY += 22;
        g.DrawString(L"Y:", -1, &lab, PointF((REAL)insX, (REAL)insY), &white);
        g.DrawString(std::to_wstring(si.y).c_str(), -1, &lab, PointF((REAL)insX+20, (REAL)insY), &white);
        insY += 22;
        g.DrawString(L"Z:", -1, &lab, PointF((REAL)insX, (REAL)insY), &white);
        g.DrawString(std::to_wstring(si.z).c_str(), -1, &lab, PointF((REAL)insX+20, (REAL)insY), &white);
        insY += 32;
        Font small(&ff, 10.0f, FontStyleRegular, UnitPixel);
        g.DrawString(L"Edit values by typing into console (TODO: GUI fields)", -1, &small, PointF((REAL)insX, (REAL)insY), &white);
    } else {
        Font lab(&ff, 11.0f, FontStyleRegular, UnitPixel);
        g.DrawString(L"No selection", -1, &lab, PointF((REAL)(winW-rightW)+12, (REAL)toolbarH+36), &white);
    }
}

// ----------------- Mouse handling -----------------

static bool PointInRect(int px, int py, int rx, int ry, int rw, int rh) {
    return (px>=rx && px<=rx+rw && py>=ry && py<=ry+rh);
}

static void OnLButtonDown(int mx, int my) {
    // check toolbar Save/Play
    if (my >= 6 && my <= 6+32 && mx >= 220 && mx <= 220+96) {
        SaveScene();
        AppendLog("User clicked Save");
        InvalidateRect(hwndGlobal, NULL, TRUE);
        return;
    }
    // compute scene area origin
    int sceneX = leftW;
    int sceneY = toolbarH;
    int sceneW = winW - leftW - rightW;
    int sceneH = winH - toolbarH - assetsH;
    // click inside scene?
    if (PointInRect(mx,my, sceneX, sceneY, sceneW, sceneH)) {
        // translate
        int sx = mx - sceneX;
        int sy = my - sceneY;
        // find topmost entity under click (reverse z)
        int hit = -1;
        for (int i = (int)entities.size()-1; i>=0; --i) {
            Entity &e = entities[i];
            int ex = e.x, ey = e.y;
            int ew = (e.assetPtr? (int)e.assetPtr->w : 48);
            int eh = (e.assetPtr? (int)e.assetPtr->h : 48);
            if (sx >= ex && sx <= ex+ew && sy >= ey && sy <= ey+eh) { hit = i; break; }
        }
        if (hit!=-1) {
            selectedEntity = hit;
            AppendLog("Selected entity (scene click): " + entities[hit].id);
            InvalidateRect(hwndGlobal, NULL, TRUE);
            return;
        } else {
            selectedEntity = -1;
            InvalidateRect(hwndGlobal, NULL, TRUE);
            return;
        }
    }
    // click in hierarchy (left)
    int hy = toolbarH + 36;
    for (size_t i=0;i<entities.size();++i) {
        int itemY = hy + (int)i * 28;
        if (PointInRect(mx,my, 8, itemY, leftW-16, 24)) {
            selectedEntity = (int)i;
            AppendLog("Selected entity (hierarchy): " + entities[i].id);
            InvalidateRect(hwndGlobal, NULL, TRUE);
            return;
        }
    }
    // click in assets area - add new entity at (10,10)
    int ax = 8, ay = winH - assetsH + 36;
    int thumb = 64, gap=8;
    int col = (mx - ax) / (thumb + gap);
    if (mx >= ax && my >= ay) {
        int index = (my - ay) / (thumb + gap) * 2 + col;
        if (index >=0 && index < (int)assets.size()) {
            // create new entity
            Entity ne;
            ne.id = "entity_" + std::to_string(entities.size()+1);
            ne.asset = assets[index].name;
            ne.x = 20; ne.y = 20; ne.z = 0;
            ne.assetPtr = &assets[index];
            entities.push_back(ne);
            selectedEntity = (int)entities.size()-1;
            AppendLog("Created entity from asset: " + WToUtf8(ne.asset));
            InvalidateRect(hwndGlobal, NULL, TRUE);
            return;
        }
    }
}

// ----------------- WinProc -----------------

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_SIZE:
        winW = LOWORD(lParam); winH = HIWORD(lParam);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintAll(hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        OnLButtonDown(mx, my);
        return 0;
    }
    case WM_KEYDOWN: {
        if (wParam == 'S') { SaveScene(); AppendLog("Saved via S"); InvalidateRect(hWnd, NULL, TRUE); }
        if (wParam == 'P') {
            // launch runtime next to EXE if exists
            std::wstring rt = exeFolder + L"\\ThunderzRuntime.exe";
            STARTUPINFOW si = {}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            std::wstring cmd = L"\"" + rt + L"\" \"" + projectPath + L"\"";
            if (CreateProcessW(rt.c_str(), (LPWSTR)cmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                AppendLog("Launched runtime: " + WToUtf8(rt));
            } else {
                AppendLog("Failed to launch runtime: " + WToUtf8(rt));
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// ----------------- Init & main -----------------

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // clear & init log
    {
        std::ofstream f("editor.log", std::ios::trunc);
        f << CurrentTimestamp() << " - --- Thunderz Editor (debug) startup ---\n";
    }

    exeFolder = GetExeFolder();
    AppendLog("EXE folder: " + WToUtf8(exeFolder));
    // read config.txt
    std::wstring cfg = exeFolder + L"\\config.txt";
    std::string content;
    if (ReadFileUtf8(cfg, content)) {
        AppendLog("Read config: " + content.substr(0, std::min<size_t>(content.size(), 1024)));
        // parse project_path line
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss,line)) {
            if (line.rfind("project_path=",0)==0) {
                std::string val = line.substr(strlen("project_path="));
                projectPath = Utf8ToW(val);
                AppendLog("Resolved project path: " + WToUtf8(projectPath));
                break;
            }
        }
    } else {
        AppendLog("config.txt missing next to exe. Using exeFolder as project path.");
        projectPath = exeFolder;
    }

    // init GDI+
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&gdiToken, &gsi, NULL) != Ok) {
        MessageBoxW(NULL, L"Gdiplus failed to initialize", L"Error", MB_ICONERROR);
        return 1;
    }
    gdiInited = true;
    AppendLog("GDI+ initialized");

    // scan/load project
    ScanAssetsFolder();
    LoadBitmaps();
    ParseScene();

    // register windows class
    const wchar_t CLASS_NAME[] = L"ThunderzEditorClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Thunderz Editor", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, NULL, NULL, hInstance, NULL);
    if (!hwnd) { AppendLog("CreateWindowExW failed"); return 1; }
    hwndGlobal = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // main loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // cleanup
    for (auto &a: assets) if (a.bmp) { delete a.bmp; a.bmp=nullptr; }
    if (gdiInited) GdiplusShutdown(gdiToken);

    AppendLog("Editor exiting");
    return 0;
}

