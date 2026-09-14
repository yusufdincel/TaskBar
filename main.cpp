#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <vector>
#include <string>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d2d1.lib")

// 🌍 Global Değişkenler
HWND g_hDock = NULL;
ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;

// 📦 Uygulama Yapısı
struct AppItem {
    HWND hwnd;
    D2D1::ColorF color;
    float currentSize;
    float targetSize;
    float xOffset;
};

// 📋 Listeler
std::vector<AppItem> openApps;
std::vector<AppItem> tempApps;

// 🛡️ Görev Çubuğu Koruyucu
class TaskbarGuard {
public:
    TaskbarGuard() { Toggle(SW_HIDE); }
    ~TaskbarGuard() { Toggle(SW_SHOW); }
private:
    void Toggle(int state) {
        HWND h1 = FindWindow(L"Shell_TrayWnd", NULL);
        if (h1) ShowWindow(h1, state);
    }
};

// 🎨 Sabit Renk Üretici
D2D1::ColorF GetColorFromHWND(HWND hwnd) {
    size_t hash = reinterpret_cast<size_t>(hwnd);
    float r = (hash % 255) / 255.0f;
    float g = ((hash >> 4) % 255) / 255.0f;
    float b = ((hash >> 8) % 255) / 255.0f;
    return D2D1::ColorF(r, g, b);
}

// 🪟 Açık Pencereleri Tara
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    
    HWND owner = GetWindow(hwnd, GW_OWNER);
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (owner != NULL || (exStyle & WS_EX_TOOLWINDOW)) return TRUE;

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;

    WCHAR title[256];
    GetWindowText(hwnd, title, 256);
    if (wcslen(title) == 0 || wcscmp(title, L"Dock") == 0 || wcscmp(title, L"Program Manager") == 0) return TRUE;

    AppItem app = { hwnd, GetColorFromHWND(hwnd), 40.0f, 40.0f, 0.0f };
    tempApps.push_back(app);
    return TRUE;
}

// 🔄 Akıllı Liste Yenileme (Sırayı Korur)
void RefreshOpenApps(HWND hwnd) {
    tempApps.clear();
    EnumWindows(EnumWindowsProc, 0);

    bool changed = false;

    for (auto it = openApps.begin(); it != openApps.end(); ) {
        bool found = false;
        for (auto& temp : tempApps) {
            if (it->hwnd == temp.hwnd) { found = true; break; }
        }
        if (!found) {
            it = openApps.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    for (auto& temp : tempApps) {
        bool found = false;
        for (auto& open : openApps) {
            if (open.hwnd == temp.hwnd) { found = true; break; }
        }
        if (!found) {
            openApps.push_back(temp);
            changed = true;
        }
    }

    if (changed && openApps.size() > 0) {
        float iconGap = 55.0f;
        float padding = 40.0f;
        int targetWidth = (openApps.size() * iconGap) + padding;

        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        SetWindowPos(hwnd, NULL, (sw - targetWidth) / 2, sh - 70 - 10, targetWidth, 70, SWP_NOZORDER | SWP_NOACTIVATE);
        if (pRenderTarget) {
            pRenderTarget->Resize(D2D1::SizeU(targetWidth, 70));
        }
    }
}

// 🛠️ Direct2D Motoru
void InitD2D(HWND hwnd) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    RECT rc; GetClientRect(hwnd, &rc);
    pFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
        &pRenderTarget
    );
}

// 🎥 Çizim Döngüsü
void Render() {
    if (!pRenderTarget) return;

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0.0f)); 

    auto size = pRenderTarget->GetSize();
    float iconGap = 55.0f;
    float totalBlockWidth = openApps.size() * iconGap;
    
    float startX = (size.width - totalBlockWidth) / 2.0f + (iconGap / 2.0f);
    float centerY = size.height / 2.0f;

    for (size_t i = 0; i < openApps.size(); i++) {
        openApps[i].xOffset = startX + (i * iconGap);
        openApps[i].currentSize += (openApps[i].targetSize - openApps[i].currentSize) * 0.15f; 

        ID2D1SolidColorBrush* pBrush;
        pRenderTarget->CreateSolidColorBrush(openApps[i].color, &pBrush);

        D2D1_ROUNDED_RECT icon = D2D1::RoundedRect(
            D2D1::RectF(openApps[i].xOffset - openApps[i].currentSize/2, centerY - openApps[i].currentSize/2, 
                        openApps[i].xOffset + openApps[i].currentSize/2, centerY + openApps[i].currentSize/2),
            12.0f, 12.0f
        );
        
        pRenderTarget->FillRoundedRectangle(icon, pBrush);
        pBrush->Release();
    }
    pRenderTarget->EndDraw();
}

// 🕹️ Pencere Olay Yöneticisi
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            InitD2D(hwnd);
            SetTimer(hwnd, 1, 1000, NULL); 
            RefreshOpenApps(hwnd);
            return 0;
            
        case WM_TIMER:
            if (wParam == 1) RefreshOpenApps(hwnd);
            return 0;
            
        case WM_MOUSEMOVE: {
            float mouseX = LOWORD(lParam);
            for (auto& app : openApps) {
                if (std::abs(mouseX - app.xOffset) < 30.0f) app.targetSize = 55.0f;
                else app.targetSize = 40.0f;
            }
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            for (auto& app : openApps) app.targetSize = 40.0f;
            return 0;
        
        case WM_LBUTTONDOWN: {
            float mouseX = LOWORD(lParam);
            for (auto& app : openApps) {
                if (std::abs(mouseX - app.xOffset) < (app.currentSize / 2.0f)) {
                    if (IsIconic(app.hwnd)) ShowWindow(app.hwnd, SW_RESTORE);
                    SetForegroundWindow(app.hwnd);
                }
            }
            return 0;
        }
            
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            if (pRenderTarget) pRenderTarget->Release();
            if (pFactory) pFactory->Release();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 🚀 Ana Başlatıcı
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    TaskbarGuard guard; 
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MacDock";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    g_hDock = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, 
        wc.lpszClassName, L"Dock", WS_POPUP,
        -2000, -2000, 100, 70, // Görünmez alan başlangıcı
        NULL, NULL, hInst, NULL
    );

    SetLayeredWindowAttributes(g_hDock, 0, 255, LWA_ALPHA);
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hDock, &margins);

    DWORD darkMode = 1; 
    DwmSetWindowAttribute(g_hDock, 20, &darkMode, sizeof(darkMode)); 
    DWORD backdrop = 2; 
    DwmSetWindowAttribute(g_hDock, 38, &backdrop, sizeof(backdrop)); 
    DWORD corners = 2; 
    DwmSetWindowAttribute(g_hDock, 33, &corners, sizeof(corners)); 

    ShowWindow(g_hDock, SW_SHOW);
    MSG msg;
    while (true) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            ShowWindow(g_hDock, SW_SHOWNA);
            Render();
            Sleep(16);
        }
    }
    return 0;
}