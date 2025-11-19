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
                if(name.find(L".bmp") != std::wstring::npos) {
                    assetFiles.push_back(name);
                }
            }
        } while(FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void LoadScene() {
    std::wstring scenePath = PROJECT_PATH + L"\\scenes\\main.scene";

    std::ifstream f(scenePath);
    if(!f) return;

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string s = buffer.str();

    auto get = [&](const std::string& key, const std::string& line) -> std::string {
        size_t p = line.find(key);
        if(p == std::string::npos) return "";
        size_t q = line.find("\"", p + key.size());
        size_t r = line.find("\"", q + 1);
        return line.substr(q+1, r-q-1);
    };

    auto getNum = [&](const std::string& key, const std::string& line) -> int {
        size_t p = line.find(key);
        if(p == std::string::npos) return 0;
        size_t q = line.find(":", p);
        size_t r = line.find(",", q);
        return std::stoi(line.substr(q+1, r-q-1));
    };

    std::stringstream ss(s);
    std::string line;

    while(std::getline(ss, line)) {
        if(line.find("\"id\"") != std::string::npos &&
           line.find("main") == std::string::npos &&
           line.find("{") != std::string::npos)
        {
            Entity e;
            std::getline(ss, line);
            e.id = ToW(get("id", line));

            std::getline(ss, line);
            e.asset = ToW(get("asset", line));

            std::getline(ss, line);
            e.x = getNum("x", line);

            std::getline(ss, line);
            e.y = getNum("y", line);

            std::getline(ss, line);
            e.z = getNum("z", line);

            entities.push_back(e);
        }
    }

    std::sort(entities.begin(), entities.end(), 
        [](const Entity& a, const Entity& b){
            return a.z < b.z;
        });
}

void LoadImages() {
    std::wstring ap = PROJECT_PATH + L"\\assets\\";

    for(auto& a : assetFiles) {
        Image* img = new Image((ap + a).c_str());
        loadedImages.push_back(img);
    }
}

Image* FindImage(const std::wstring& name) {
    for(size_t i = 0; i < assetFiles.size(); i++) {
        if(assetFiles[i] == name)
            return loadedImages[i];
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

            SolidBrush bg(Color(255, 80, 100, 160));
            g.FillRectangle(&bg, 0, 0, 2000, 2000);

            for(auto& e : entities) {
                Image* im = FindImage(e.asset);
                if(im)
                    g.DrawImage(im, e.x, e.y);
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

int APIENTRY wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int) {
    GdiplusStartupInput gi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gi, NULL);

    LoadConfig();
    ScanAssets();
    LoadScene();
    LoadImages();

    WNDCLASSW wc = {0};
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.hInstance = h;
    wc.lpszClassName = L"RT";
    wc.lpfnWndProc = WndProc;

    RegisterClassW(&wc);

    HWND win = CreateWindowW(L"RT", L"Thunderz Runtime",
                             WS_OVERLAPPEDWINDOW,
                             10, 10, 1280, 720,
                             NULL, NULL, h, NULL);

    ShowWindow(win, 1);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
