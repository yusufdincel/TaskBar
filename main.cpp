#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <propidl.h>
#include <vector>
#include <string>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

const PROPERTYKEY PKEY_AppUserModel_ID_Local = { {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5 };
const GUID IID_IPropertyStore_Local = { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };
const GUID IID_IShellItem_Local = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
const GUID IID_IShellItemImageFactory_Local = { 0xbcc18b79, 0xba16, 0x442f, { 0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b } };

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

ID2D1Bitmap* GetUWPAppIcon(HWND hwnd) {
    if (!pWicFactory || !pRenderTarget) return nullptr;

    IPropertyStore* pps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore_Local, (void**)&pps))) return nullptr;

    PROPVARIANT prop = {}; 
    HRESULT hr = pps->GetValue(PKEY_AppUserModel_ID_Local, &prop);
    ID2D1Bitmap* pD2dBitmap = nullptr;
    
    if (SUCCEEDED(hr) && prop.vt == VT_LPWSTR) {
        std::wstring parseName = L"shell:AppsFolder\\";
        parseName += prop.pwszVal;

        IShellItem* pItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(parseName.c_str(), nullptr, IID_IShellItem_Local, (void**)&pItem))) {
            IShellItemImageFactory* pFactory = nullptr;
            if (SUCCEEDED(pItem->QueryInterface(IID_IShellItemImageFactory_Local, (void**)&pFactory))) {
                HBITMAP hbmp = nullptr;
                SIZE size = { 64, 64 };
                if (SUCCEEDED(pFactory->GetImage(size, SIIGBF_ICONONLY, &hbmp))) {
                    IWICBitmap* pWicBitmap = nullptr;
                    if (SUCCEEDED(pWicFactory->CreateBitmapFromHBITMAP(hbmp, NULL, WICBitmapUseAlpha, &pWicBitmap))) {
                        IWICFormatConverter* pConverter = nullptr;
                        pWicFactory->CreateFormatConverter(&pConverter);
                        pConverter->Initialize(pWicBitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeMedianCut);
                        pRenderTarget->CreateBitmapFromWicBitmap(pConverter, NULL, &pD2dBitmap);
                        pConverter->Release();
                        pWicBitmap->Release();
                    }
                    DeleteObject(hbmp);
                }
                pFactory->Release();
            }
            pItem->Release();
        }
        PropVariantClear(&prop);
    }
    pps->Release();
    return pD2dBitmap;
}

ID2D1Bitmap* GetAppBitmap(HWND hwnd) {
    ID2D1Bitmap* pBitmap = GetUWPAppIcon(hwnd);
    if (pBitmap) return pBitmap;

    HICON hIcon = NULL;
    SendMessageTimeout(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICON);
    if (!hIcon) SendMessageTimeout(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
    if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICONSM);
    
    if (hIcon) return HIconToBitmap(hIcon);
    return nullptr;
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

    AppItem app = { hwnd, nullptr, GetColorFromHWND(hwnd), 32.0f, 32.0f, 0.0f };
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
            temp.pBitmap = GetAppBitmap(temp.hwnd);
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

    float baseSize = 32.0f;
    float maxSize = 80.0f;
    float gap = 10.0f;
    float padding = 16.0f;
    float effectRadius = 120.0f;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    
    // 🪄 MATEMATİK DÜZELTMESİ: Pencere genişlese bile ikonların "orijinal" statik merkezini buluyoruz.
    float unmagTotalWidth = padding + (openApps.size() * (baseSize + gap)) + padding;
    float fixedLeft = (sw - unmagTotalWidth) / 2.0f; 

    float currentTotalWidth = padding;

    for (size_t i = 0; i < openApps.size(); i++) {
        // 🪄 Farenin hedef alacağı merkez artık mutlak (ekrana sabit). Hiç kaymayacak!
        float unmagCenter = fixedLeft + padding + 5.0f + (i * (baseSize + gap)) + (baseSize / 2.0f);
        
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
    int windowHeight = 100; 
    int currentWindowWidth = windowRect.right - windowRect.left;
    
    if (std::abs(targetWindowWidth - currentWindowWidth) > 1 && openApps.size() > 0) {
        SetWindowPos(g_hDock, NULL, (sw - targetWindowWidth) / 2, GetSystemMetrics(SM_CYSCREEN) - windowHeight, targetWindowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        pRenderTarget->Resize(D2D1::SizeU(targetWindowWidth, windowHeight));
    }

    pRenderTarget->BeginDraw();
    pRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0.0f)); 

    auto size = pRenderTarget->GetSize();
    
    float barHeight = 46.0f; 
    float bottomMargin = 1.0f; 
    float barTop = size.height - barHeight - bottomMargin;
    
    ID2D1SolidColorBrush* pDockBrush;
    pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.5f), &pDockBrush); 
    D2D1_ROUNDED_RECT dockRect = D2D1::RoundedRect(D2D1::RectF(5, barTop, size.width - 5, size.height - bottomMargin), 14.0f, 14.0f);
    pRenderTarget->FillRoundedRectangle(dockRect, pDockBrush);
    pDockBrush->Release();

    float currentX = padding + 5.0f;
    float bottomY = size.height - bottomMargin - ((barHeight - baseSize) / 2.0f); 

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
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), pFallbackBrush);
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