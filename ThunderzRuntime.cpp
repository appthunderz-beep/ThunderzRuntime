#define UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ------------------- DATA STRUCTURES -------------------

struct Entity {
    std::wstring id;
    std::wstring asset;
    int x, y, z;
};

std::wstring PROJECT_PATH;
std::vector<Entity> entities;
std::vector<std::wstring> assetFiles;
std::vector<Image*> loadedImages;

// ------------------- HELPERS -------------------

std::wstring ToW(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::string ReadFileToStringW(const std::wstring& path) {
    // Open wide file handle
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return std::string();

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart == 0) {
        CloseHandle(h);
        return std::string();
    }

    DWORD fileSize = (DWORD)sz.QuadPart;
    std::string buffer;
    buffer.resize(fileSize);

    DWORD read = 0;
    if (!ReadFile(h, &buffer[0], fileSize, &read, NULL) || read != fileSize) {
        CloseHandle(h);
        return std::string();
    }

    CloseHandle(h);
    return buffer;
}

void LoadConfig() {
    std::ifstream f("config.txt");
    if(!f) return;

    std::string line;
    std::getline(f, line);

    if(line.rfind("project_path=", 0) == 0) {
        PROJECT_PATH = ToW(line.substr(13));
    }
}

void ScanAssets() {
    std::wstring ap = PROJECT_PATH + L"\\assets\\";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((ap + L"*").c_str(), &fd);

    if(h != INVALID_HANDLE_VALUE) {
        do {
            if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring name = fd.cFileName;
                // only add bitmap files (simple filter)
                if(name.size() > 4) {
                    std::wstring ext = name.substr(name.size()-4);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                    if(ext == L".bmp") {
                        assetFiles.push_back(name);
                    }
                }
            }
        } while(FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void LoadScene() {
    std::wstring scenePath = PROJECT_PATH + L"\\scenes\\main.scene";
    std::string s = ReadFileToStringW(scenePath);
    if(s.empty()) return;

    // Simple minimal JSON-ish parser tuned for the project's scene format
    auto getQuoted = [&](const std::string& line, const std::string& key)->std::string{
        size_t p = line.find(key);
        if(p == std::string::npos) return "";
        size_t q = line.find("\"", p + key.size());
        if(q == std::string::npos) return "";
        size_t r = line.find("\"", q + 1);
        if(r == std::string::npos) return "";
        return line.substr(q+1, r-q-1);
    };

    auto getNumber = [&](const std::string& line, const std::string& key)->int{
        size_t p = line.find(key);
        if(p == std::string::npos) return 0;
        size_t q = line.find(":", p);
        if(q == std::string::npos) return 0;
        size_t r = line.find_first_of(",}", q+1);
        std::string num = line.substr(q+1, r-q-1);
        // trim
        size_t a = num.find_first_not_of(" \t\r\n");
        size_t b = num.find_last_not_of(" \t\r\n");
        if(a==std::string::npos) return 0;
        return std::stoi(num.substr(a, b-a+1));
    };

    std::stringstream ss(s);
    std::string line;
    while(std::getline(ss, line)) {
        // detect entity entry by looking for '{' followed by the keys; this is simple but matches our format
        if(line.find("{") != std::string::npos) {
            // peek next few lines for id/asset/x/y/z
            std::streampos start = ss.tellg();
            std::string l1, l2, l3, l4, l5;
            if(std::getline(ss, l1) && std::getline(ss, l2) && std::getline(ss, l3) && std::getline(ss, l4) && std::getline(ss, l5)) {
                // l1 should contain "id": ...
                if(l1.find("\"id\"") != std::string::npos && l2.find("\"asset\"") != std::string::npos) {
                    Entity e;
                    e.id = ToW(getQuoted(l1, "\"id\""));
                    e.asset = ToW(getQuoted(l2, "\"asset\""));
                    e.x = getNumber(l3, "x");
                    e.y = getNumber(l4, "y");
                    e.z = getNumber(l5, "z");
                    entities.push_back(e);
                } else {
                    // not an entity; rewind a bit by resetting stringstream to after '{'
                    ss.seekg(start);
                }
            } else {
                // fewer lines — nothing to do
                break;
            }
        }
    }

    std::sort(entities.begin(), entities.end(), [](const Entity& a, const Entity& b){
        return a.z < b.z;
    });
}

void LoadImages() {
    std::wstring ap = PROJECT_PATH + L"\\assets\\";
    for(auto& a : assetFiles) {
        // Image accepts LPCWSTR
        Image* img = new Image((ap + a).c_str());
        loadedImages.push_back(img);
    }
}

Image* FindImage(const std::wstring& name) {
    for(size_t i = 0; i < assetFiles.size(); ++i) {
        if(assetFiles[i] == name) return loadedImages[i];
    }
    return nullptr;
}

// ------------------- RENDER LOOP -------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Graphics g(hdc);

            // background (same blue-ish tone)
            SolidBrush bg(Color(255, 70, 110, 170));
            g.FillRectangle(&bg, 0, 0, 2000, 2000);

            // draw entities
            for(auto& e : entities) {
                Image* im = FindImage(e.asset);
                if(im) {
                    g.DrawImage(im, e.x, e.y);
                } else {
                    // draw a placeholder if image missing
                    SolidBrush rb(Color(255, 200, 60, 60));
                    g.FillRectangle(&rb, e.x, e.y, 32, 32);
                }
            }

            EndPaint(hwnd, &ps);
        } return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ------------------- ENTRY -------------------

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    GdiplusStartupInput gi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gi, NULL);

    LoadConfig();
    ScanAssets();
    LoadScene();
    LoadImages();

    WNDCLASSW wc = {0};
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ThunderzRuntimeClass";
    wc.lpfnWndProc = WndProc;

    RegisterClassW(&wc);

    HWND win = CreateWindowW(L"ThunderzRuntimeClass", L"Thunderz Runtime",
                             WS_OVERLAPPEDWINDOW,
                             10, 10, 1280, 720,
                             NULL, NULL, hInstance, NULL);

    ShowWindow(win, SW_SHOWDEFAULT);
    UpdateWindow(win);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // cleanup images
    for(auto p : loadedImages) delete p;
    GdiplusShutdown(token);
    return (int)msg.wParam;
}

