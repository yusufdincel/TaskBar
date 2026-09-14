#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <wincodec.h>
#include <vector>
#include <string>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

HWND g_hDock = NULL;
ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
IWICImagingFactory* pWicFactory = nullptr;

struct AppItem {
    HWND hwnd;
    ID2D1Bitmap* pBitmap;
    D2D1::ColorF fallbackColor; 
    float currentSize;
    float targetSize;
    float xOffset;
};

std::vector<AppItem> openApps;
std::vector<AppItem> tempApps;

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

D2D1::ColorF GetColorFromHWND(HWND hwnd) {
    size_t hash = reinterpret_cast<size_t>(hwnd);
    return D2D1::ColorF((hash % 255)/255.0f, ((hash >> 4) % 255)/255.0f, ((hash >> 8) % 255)/255.0f);
}

HICON GetWindowIcon(HWND hwnd) {
    HICON hIcon = NULL;
    SendMessageTimeout(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICON);
    if (!hIcon) SendMessageTimeout(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICONSM);
    return hIcon;
}

ID2D1Bitmap* HIconToBitmap(HICON hIcon) {
    if (!pWicFactory || !pRenderTarget || !hIcon) return nullptr;
    IWICBitmap* pWicBitmap = nullptr;
    if (FAILED(pWicFactory->CreateBitmapFromHICON(hIcon, &pWicBitmap))) return nullptr;

    IWICFormatConverter* pConverter = nullptr;
    pWicFactory->CreateFormatConverter(&pConverter);
    pConverter->Initialize(pWicBitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeMedianCut);

    ID2D1Bitmap* pD2dBitmap = nullptr;
    pRenderTarget->CreateBitmapFromWicBitmap(pConverter, NULL, &pD2dBitmap);

    pConverter->Release();
    pWicBitmap->Release();
    return pD2dBitmap;
}

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

    AppItem app = { hwnd, nullptr, GetColorFromHWND(hwnd), 40.0f, 40.0f, 0.0f };
    tempApps.push_back(app);
    return TRUE;
}

void RefreshOpenApps() {
    tempApps.clear();
    EnumWindows(EnumWindowsProc, 0);

    for (auto it = openApps.begin(); it != openApps.end(); ) {
        bool found = false;
        for (auto& temp : tempApps) {
            if (it->hwnd == temp.hwnd) { found = true; break; }
        }
        if (!found) {
            if (it->pBitmap) it->pBitmap->Release();
            it = openApps.erase(it);
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
            temp.pBitmap = HIconToBitmap(GetWindowIcon(temp.hwnd));
            openApps.push_back(temp);
        }
    }
}

void InitD2D(HWND hwnd) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWicFactory));
    
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    RECT rc; GetClientRect(hwnd, &rc);
    pFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
        &pRenderTarget
    );
    RefreshOpenApps();
}

void Render() {
    if (!pRenderTarget) return;

    POINT pt; GetCursorPos(&pt);
    RECT windowRect; GetWindowRect(g_hDock, &windowRect);
    bool isHovering = (pt.y >= windowRect.top && pt.y <= windowRect.bottom);

    float baseSize = 40.0f;
    float maxSize = 90.0f;
    float gap = 12.0f;
    float padding = 20.0f;
    float effectRadius = 140.0f;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    float currentTotalWidth = padding;

    for (size_t i = 0; i < openApps.size(); i++) {
        float unmagCenter = windowRect.left + padding + (i * (baseSize + gap)) + (baseSize / 2.0f);
        
        if (isHovering) {
            float distance = std::abs(pt.x - unmagCenter);
            if (distance < effectRadius) {
                float ratio = std::pow(std::cos((distance / effectRadius) * (3.14159f / 2.0f)), 1.5f);
                openApps[i].targetSize = baseSize + ((maxSize - baseSize) * ratio);
            } else {
                openApps[i].targetSize = baseSize;
            }
        } else {
            openApps[i].targetSize = baseSize;
        }
        
        openApps[i].currentSize += (openApps[i].targetSize - openApps[i].currentSize) * 0.25f;
        currentTotalWidth += openApps[i].currentSize + gap;
    }
    
    int targetWindowWidth = static_cast<int>(currentTotalWidth + padding);
    int windowHeight = 120; // 🪄 Sabit yükseklik, devasa bloğu engeller
    int currentWindowWidth = windowRect.right - windowRect.left;
    
    if (std::abs(targetWindowWidth - currentWindowWidth) > 1 && openApps.size() > 0) {
        SetWindowPos(g_hDock, NULL, (sw - targetWindowWidth) / 2, GetSystemMetrics(SM_CYSCREEN) - windowHeight - 10, targetWindowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        pRenderTarget->Resize(D2D1::SizeU(targetWindowWidth, windowHeight));
    }

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0.0f)); // 🪄 Pencereyi tamamen şeffaf yapar

    auto size = pRenderTarget->GetSize();
    
    // 🪄 İnce, yarı saydam alt bar (Dock arka planı)
    float barHeight = baseSize + 20.0f;
    float barTop = size.height - barHeight - 5.0f;
    
    ID2D1SolidColorBrush* pDockBrush;
    pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.5f), &pDockBrush); 
    D2D1_ROUNDED_RECT dockRect = D2D1::RoundedRect(D2D1::RectF(5, barTop, size.width - 5, size.height - 5), 18.0f, 18.0f);
    pRenderTarget->FillRoundedRectangle(dockRect, pDockBrush);
    pDockBrush->Release();

    float currentX = padding + 5.0f;
    float bottomY = size.height - 15.0f; // 🪄 İkonları zemine yapıştırır

    for (size_t i = 0; i < openApps.size(); i++) {
        openApps[i].xOffset = currentX + (openApps[i].currentSize / 2.0f);
        
        D2D1_RECT_F rect = D2D1::RectF(
            openApps[i].xOffset - openApps[i].currentSize / 2.0f,
            bottomY - openApps[i].currentSize, 
            openApps[i].xOffset + openApps[i].currentSize / 2.0f,
            bottomY
        );

        if (openApps[i].pBitmap) {
            pRenderTarget->DrawBitmap(openApps[i].pBitmap, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            ID2D1SolidColorBrush* pFallbackBrush;
            pRenderTarget->CreateSolidColorBrush(openApps[i].fallbackColor, &pFallbackBrush);
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, 8.0f, 8.0f), pFallbackBrush);
            pFallbackBrush->Release();
        }
        
        currentX += openApps[i].currentSize + gap;
    }
    pRenderTarget->EndDraw();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            InitD2D(hwnd);
            SetTimer(hwnd, 1, 1000, NULL); 
            return 0;
            
        case WM_TIMER:
            if (wParam == 1) RefreshOpenApps();
            return 0;
        
        case WM_LBUTTONDOWN: {
            POINT pt; GetCursorPos(&pt);
            RECT rc; GetWindowRect(hwnd, &rc);
            float mouseX = pt.x - rc.left; 
            
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
            for(auto& app : openApps) if(app.pBitmap) app.pBitmap->Release();
            if (pRenderTarget) pRenderTarget->Release();
            if (pFactory) pFactory->Release();
            if (pWicFactory) pWicFactory->Release();
            CoUninitialize();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

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
        -2000, -2000, 100, 100, 
        NULL, NULL, hInst, NULL
    );

    SetLayeredWindowAttributes(g_hDock, 0, 255, LWA_ALPHA);
    
    // 🪄 Şeffaflığı sağlayan çekirdek katman (Blur efekti kaldırıldı)
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hDock, &margins);

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