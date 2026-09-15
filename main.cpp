#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <propidl.h>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

const PROPERTYKEY PKEY_AppUserModel_ID_Local = { {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5 };
const GUID IID_IPropertyStore_Local = { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };
const GUID IID_IShellItem_Local = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
const GUID IID_IShellItemImageFactory_Local = { 0xbcc18b79, 0xba16, 0x442f, { 0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b } };

HWND g_hDock = NULL;
HWND g_hMenu = NULL;
ID2D1Factory* pFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
ID2D1HwndRenderTarget* pMenuRT = nullptr;
IWICImagingFactory* pWicFactory = nullptr;
IDWriteFactory* pDWriteFactory = nullptr;
IDWriteTextFormat* pTextFormat = nullptr;

float g_expansion = 0.0f; 
D2D1_RECT_F g_dockRect = {0}; 

std::vector<std::pair<std::wstring, bool>> g_pinnedApps;

struct AppItem {
    HWND hwnd;
    std::wstring id;
    bool isUWP;
    bool isPinned;
    ID2D1Bitmap* pBitmap;
    D2D1::ColorF fallbackColor; 
    float currentSize;
    float targetSize;
    float xOffset;
};

std::vector<AppItem> openApps;

struct MenuItem {
    int id;
    std::wstring text;
};
std::vector<MenuItem> g_menuItems;
int g_hoveredMenuItem = -1;

std::wstring g_menuAppId;
HWND g_menuAppHwnd = NULL;
bool g_menuAppIsPinned = false;
bool g_menuAppIsUWP = false;

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

void LoadPinnedApps() {
    g_pinnedApps.clear();
    FILE* f = _wfopen(L"dock_pinned.txt", L"r, ccs=UTF-8");
    if (f) {
        WCHAR line[512];
        while (fgetws(line, 512, f)) {
            std::wstring s = line;
            while(!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            if (!s.empty()) {
                bool uwp = (s.substr(0, 4) == L"UWP|");
                g_pinnedApps.push_back({s.substr(4), uwp});
            }
        }
        fclose(f);
    }
}

void SavePinnedApps() {
    FILE* f = _wfopen(L"dock_pinned.txt", L"w, ccs=UTF-8");
    if (f) {
        for (auto& p : g_pinnedApps) {
            fwprintf(f, L"%s|%s\n", p.second ? L"UWP" : L"WIN", p.first.c_str());
        }
        fclose(f);
    }
}

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

std::wstring GetWindowIdentifier(HWND hwnd, bool& isUWP) {
    isUWP = false;
    IPropertyStore* pps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore_Local, (void**)&pps))) {
        PROPVARIANT prop = {}; 
        if (SUCCEEDED(pps->GetValue(PKEY_AppUserModel_ID_Local, &prop)) && prop.vt == VT_LPWSTR) {
            std::wstring aumid = prop.pwszVal;
            PropVariantClear(&prop); pps->Release(); isUWP = true; return aumid;
        }
        pps->Release();
    }
    DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        WCHAR path[MAX_PATH]; DWORD size = MAX_PATH;
        if (QueryFullProcessImageName(hProc, 0, path, &size)) { CloseHandle(hProc); return std::wstring(path); }
        CloseHandle(hProc);
    }
    return L"";
}

ID2D1Bitmap* GetIconFromIdentifier(const std::wstring& id, bool isUWP, HWND hwnd) {
    if (hwnd) {
        WCHAR className[256]; GetClassName(hwnd, className, 256);
        if (wcscmp(className, L"CabinetWClass") == 0) {
            HICON hFolderIcon = ExtractIcon(NULL, L"shell32.dll", 3);
            if (hFolderIcon) { ID2D1Bitmap* bmp = HIconToBitmap(hFolderIcon); DestroyIcon(hFolderIcon); return bmp; }
        }
    }
    if (isUWP) {
        std::wstring parseName = L"shell:AppsFolder\\" + id;
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(parseName.c_str(), nullptr, IID_IShellItem_Local, (void**)&pItem))) {
            IShellItemImageFactory* pFactory = nullptr; ID2D1Bitmap* bmp = nullptr;
            if (SUCCEEDED(pItem->QueryInterface(IID_IShellItemImageFactory_Local, (void**)&pFactory))) {
                HBITMAP hbmp = nullptr; SIZE size = { 64, 64 };
                if (SUCCEEDED(pFactory->GetImage(size, SIIGBF_ICONONLY, &hbmp))) {
                    IWICBitmap* pWicBitmap = nullptr;
                    if (SUCCEEDED(pWicFactory->CreateBitmapFromHBITMAP(hbmp, NULL, WICBitmapUseAlpha, &pWicBitmap))) {
                        IWICFormatConverter* pConverter = nullptr; pWicFactory->CreateFormatConverter(&pConverter);
                        pConverter->Initialize(pWicBitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeMedianCut);
                        pRenderTarget->CreateBitmapFromWicBitmap(pConverter, NULL, &bmp);
                        pConverter->Release(); pWicBitmap->Release();
                    } DeleteObject(hbmp);
                } pFactory->Release();
            } pItem->Release(); if (bmp) return bmp;
        }
    }
    if (hwnd) {
        HICON hIcon = NULL; SendMessageTimeout(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
        if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICON);
        if (!hIcon) SendMessageTimeout(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 50, (PDWORD_PTR)&hIcon);
        if (!hIcon) hIcon = (HICON)GetClassLongPtr(hwnd, GCLP_HICONSM);
        if (hIcon) return HIconToBitmap(hIcon);
    } else {
        SHFILEINFO sfi = {0};
        if (SHGetFileInfo(id.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)) {
            ID2D1Bitmap* bmp = HIconToBitmap(sfi.hIcon); DestroyIcon(sfi.hIcon); return bmp;
        }
    }
    return nullptr;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    HWND owner = GetWindow(hwnd, GW_OWNER); LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (owner != NULL || (exStyle & WS_EX_TOOLWINDOW)) return TRUE;
    int cloaked = 0; DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    WCHAR title[256]; GetWindowText(hwnd, title, 256);
    if (wcslen(title) == 0 || wcscmp(title, L"Dock") == 0 || wcscmp(title, L"Program Manager") == 0 || wcscmp(title, L"MacDockMenu") == 0) return TRUE;
    std::vector<HWND>* list = (std::vector<HWND>*)lParam; list->push_back(hwnd); return TRUE;
}

void RefreshOpenApps() {
    std::vector<AppItem> newApps;
    newApps.push_back({ (HWND)1, L"START", false, false, nullptr, D2D1::ColorF(0,0,0,0), 32.0f, 32.0f, 0.0f });

    for (auto& p : g_pinnedApps) {
        newApps.push_back({ NULL, p.first, p.second, true, nullptr, D2D1::ColorF(0.5f,0.5f,0.5f), 32.0f, 32.0f, 0.0f });
    }

    std::vector<HWND> openHwnds;
    EnumWindows(EnumWindowsProc, (LPARAM)&openHwnds);

    for (HWND hw : openHwnds) {
        bool isUWP; std::wstring id = GetWindowIdentifier(hw, isUWP);
        for(auto& na : newApps) {
            if (na.isPinned && na.id == id && na.hwnd == NULL) { na.hwnd = hw; break; }
        }
    }

    for (auto& oa : openApps) {
        if (oa.hwnd != (HWND)1 && !oa.isPinned) {
            bool stillOpen = false;
            for (HWND hw : openHwnds) { if (oa.hwnd == hw) { stillOpen = true; break; } }
            if (stillOpen) newApps.push_back({ oa.hwnd, oa.id, oa.isUWP, false, nullptr, oa.fallbackColor, 32.0f, 32.0f, 0.0f });
        }
    }

    for (HWND hw : openHwnds) {
        bool found = false;
        for (auto& na : newApps) { if (na.hwnd == hw) { found = true; break; } }
        if (!found) {
            bool isUWP; std::wstring id = GetWindowIdentifier(hw, isUWP);
            newApps.push_back({ hw, id, isUWP, false, nullptr, GetColorFromHWND(hw), 32.0f, 32.0f, 0.0f });
        }
    }

    for (auto& na : newApps) {
        bool foundMatch = false;
        for (auto& oa : openApps) {
            if ((na.hwnd == (HWND)1 && oa.hwnd == (HWND)1) || 
                (na.hwnd != NULL && na.hwnd != (HWND)1 && na.hwnd == oa.hwnd) || 
                (na.hwnd == NULL && oa.hwnd == NULL && na.id == oa.id)) 
            {
                na.pBitmap = oa.pBitmap; 
                if (na.pBitmap) na.pBitmap->AddRef(); 
                na.currentSize = oa.currentSize; 
                na.targetSize = oa.targetSize;
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch && na.hwnd != (HWND)1) {
            na.pBitmap = GetIconFromIdentifier(na.id, na.isUWP, na.hwnd);
        }
    }

    for (auto& oa : openApps) if (oa.pBitmap) oa.pBitmap->Release();
    openApps = newApps;
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
    
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&pDWriteFactory));
    if (pDWriteFactory) {
        pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"tr-TR", &pTextFormat);
        if (pTextFormat) {
            pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    LoadPinnedApps();
    RefreshOpenApps();
}

void Render() {
    if (!pRenderTarget) return;
    int sw = GetSystemMetrics(SM_CXSCREEN); 
    int sh = GetSystemMetrics(SM_CYSCREEN); 
    int windowHeight = 100; 

    bool isMaximized = false;
    HWND fg = GetForegroundWindow(); HWND rootFg = GetAncestor(fg, GA_ROOTOWNER); 
    if (fg && fg != g_hDock && fg != g_hMenu) {
        WCHAR className[256]; GetClassName(fg, className, 256);
        if (wcscmp(className, L"Progman") != 0 && wcscmp(className, L"WorkerW") != 0) {
            RECT fgRect; GetWindowRect(fg, &fgRect);
            if (fgRect.left <= 0 && fgRect.top <= 0 && fgRect.right >= sw && fgRect.bottom >= sh) { isMaximized = true; } 
            else { WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) }; if (GetWindowPlacement(fg, &wp) && wp.showCmd == SW_MAXIMIZE) isMaximized = true; }
        }
    }

    float targetExpansion = isMaximized ? 1.0f : 0.0f;
    g_expansion += (targetExpansion - g_expansion) * 0.15f; 

    POINT pt; GetCursorPos(&pt);
    POINT clientPt = pt; ScreenToClient(g_hDock, &clientPt);
    bool isHovering = (clientPt.y >= windowHeight - 65) && (g_hMenu == NULL);

    float baseSize = 32.0f; float maxSize = 54.0f; float gap = 12.0f; float padding = 8.0f; float effectRadius = 120.0f;
    float unmagTotalWidth = padding + (openApps.size() * (baseSize + gap)) + padding;
    float fixedLeft = (sw - unmagTotalWidth) / 2.0f; 
    
    float iconsWidth = 0.0f;

    for (size_t i = 0; i < openApps.size(); i++) {
        float unmagCenter = fixedLeft + padding + 5.0f + (i * (baseSize + gap)) + (baseSize / 2.0f);
        
        if (isHovering) {
            float distance = std::abs(pt.x - unmagCenter);
            if (distance < effectRadius) {
                float ratio = std::pow(std::cos((distance / effectRadius) * (3.14159f / 2.0f)), 1.5f);
                openApps[i].targetSize = baseSize + ((maxSize - baseSize) * ratio);
            } else { openApps[i].targetSize = baseSize; }
        } else { openApps[i].targetSize = baseSize; }
        
        float diff = openApps[i].targetSize - openApps[i].currentSize;
        if (std::abs(diff) < 0.05f) { openApps[i].currentSize = openApps[i].targetSize; } 
        else { openApps[i].currentSize += diff * 0.25f; }
        
        iconsWidth += openApps[i].currentSize;
        if (i < openApps.size() - 1) iconsWidth += gap; 
    }
    
    float totalDrawWidth = iconsWidth + (padding * 2.0f);
    float currentBarWidth = totalDrawWidth + (sw - totalDrawWidth) * g_expansion;
    float barHeight = 48.0f; 
    float bottomMargin = 6.0f * (1.0f - g_expansion); 
    float cornerRadius = 16.0f * (1.0f - g_expansion);
    float barTop = windowHeight - barHeight - bottomMargin;
    float barLeft = (sw - currentBarWidth) / 2.0f;
    float barRight = barLeft + currentBarWidth;

    g_dockRect = D2D1::RectF(barLeft, barTop, barRight, windowHeight);

    pRenderTarget->BeginDraw(); 
    pRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0.0f)); 
    
    ID2D1SolidColorBrush* pDockBrush; 
    pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.5f + (0.45f * g_expansion)), &pDockBrush); 
    D2D1_ROUNDED_RECT dockRect = D2D1::RoundedRect(D2D1::RectF(barLeft, barTop, barRight, windowHeight - bottomMargin), cornerRadius, cornerRadius);
    pRenderTarget->FillRoundedRectangle(dockRect, pDockBrush);
    
    ID2D1SolidColorBrush* pBorderBrush; 
    pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.35f * (1.0f - g_expansion)), &pBorderBrush);
    pRenderTarget->DrawRoundedRectangle(dockRect, pBorderBrush, 1.0f);
    
    pDockBrush->Release(); pBorderBrush->Release();

    float startX = (sw - iconsWidth) / 2.0f;
    float currentX = startX;
    float bottomY = windowHeight - bottomMargin - ((barHeight - baseSize) / 2.0f); 

    for (size_t i = 0; i < openApps.size(); i++) {
        openApps[i].xOffset = currentX + (openApps[i].currentSize / 2.0f);
        float cx = openApps[i].xOffset; 
        float cy = bottomY - (openApps[i].currentSize / 2.0f);
        
        float draw_cx = std::round(cx);
        float draw_cy = std::round(cy);
        float draw_size = std::round(openApps[i].currentSize);

        D2D1_RECT_F rect = D2D1::RectF(draw_cx - draw_size / 2.0f, bottomY - draw_size, draw_cx + draw_size / 2.0f, bottomY);

        if (openApps[i].hwnd == (HWND)1) {
            ID2D1SolidColorBrush* pWinBrush; 
            pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.4705f, 0.8313f), &pWinBrush); 
            
            float hg = std::round(draw_size * 0.035f); 
            float sq = std::round(draw_size * 0.35f);  
            float r  = std::round(draw_size * 0.04f);  
            
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(draw_cx - hg - sq, draw_cy - hg - sq, draw_cx - hg, draw_cy - hg), r, r), pWinBrush);
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(draw_cx + hg, draw_cy - hg - sq, draw_cx + hg + sq, draw_cy - hg), r, r), pWinBrush);
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(draw_cx - hg - sq, draw_cy + hg, draw_cx - hg, draw_cy + hg + sq), r, r), pWinBrush);
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(draw_cx + hg, draw_cy + hg, draw_cx + hg + sq, draw_cy + hg + sq), r, r), pWinBrush);
            pWinBrush->Release();
        } else if (openApps[i].pBitmap) {
            float opacity = (openApps[i].hwnd == NULL) ? 0.6f : 1.0f;
            pRenderTarget->DrawBitmap(openApps[i].pBitmap, rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            ID2D1SolidColorBrush* pFallbackBrush; pRenderTarget->CreateSolidColorBrush(openApps[i].fallbackColor, &pFallbackBrush);
            pRenderTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), pFallbackBrush);
            pFallbackBrush->Release();
        }
        
        if (openApps[i].hwnd != (HWND)1 && openApps[i].hwnd != NULL) { 
            float dotRadius = 2.0f; float dotY = bottomY + 4.5f; 
            ID2D1SolidColorBrush* pDotBrush;
            if (openApps[i].hwnd == fg || openApps[i].hwnd == rootFg) pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f), &pDotBrush); 
            else pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.75f, 0.75f, 0.8f), &pDotBrush); 
            pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(draw_cx, dotY), dotRadius, dotRadius), pDotBrush);
            pDotBrush->Release();
        }
        currentX += openApps[i].currentSize; if (i < openApps.size() - 1) currentX += gap;
    }
    pRenderTarget->EndDraw();
}

// 🎨 KUSURSUZ GÖRSEL MENU (Saydamlık ve Konum Çözüldü)
LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);

            // 🪄 YENİ: Geç Yükleme (Tuval güvenliği sağlandı)
            if (!pMenuRT) {
                RECT rc; GetClientRect(hwnd, &rc);
                pFactory->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
                    &pMenuRT
                );
            }

            if (pMenuRT && !g_menuItems.empty()) {
                pMenuRT->BeginDraw(); 
                pMenuRT->Clear(D2D1::ColorF(0, 0, 0, 0.0f));
                auto size = pMenuRT->GetSize();
                
                // Renkler resimle birebir ayarlandı
                ID2D1SolidColorBrush* pBgBrush; pMenuRT->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.95f, 0.94f, 1.0f), &pBgBrush);
                ID2D1SolidColorBrush* pBorderBrush; pMenuRT->CreateSolidColorBrush(D2D1::ColorF(0.88f, 0.87f, 0.85f, 1.0f), &pBorderBrush);
                ID2D1SolidColorBrush* pTextBrush; pMenuRT->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f), &pTextBrush);
                ID2D1SolidColorBrush* pHoverBrush; pMenuRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f), &pHoverBrush);
                
                D2D1_ROUNDED_RECT rect = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), 8.0f, 8.0f);
                pMenuRT->FillRoundedRectangle(rect, pBgBrush); 
                pMenuRT->DrawRoundedRectangle(rect, pBorderBrush, 1.0f);
                
                float itemHeight = 36.0f;
                float currentY = 8.0f; 
                
                for (size_t i = 0; i < g_menuItems.size(); i++) {
                    D2D1_RECT_F itemRect = D2D1::RectF(6, currentY, size.width - 6, currentY + itemHeight);
                    if (g_hoveredMenuItem == i) {
                        pMenuRT->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 4.0f, 4.0f), pHoverBrush);
                    }
                    
                    // 🪄 Vektörel Manuel Çizim (Hata Riskini %0'a indirir)
                    float cx = 22.0f; 
                    float cy = currentY + (itemHeight / 2.0f);
                    int id = g_menuItems[i].id;
                    
                    if (id == 1) { // Ayarlar (Dişli çark benzeri daire)
                        pMenuRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), pTextBrush, 1.5f);
                        pMenuRT->DrawLine(D2D1::Point2F(cx, cy-5.0f), D2D1::Point2F(cx, cy+5.0f), pTextBrush, 1.5f);
                        pMenuRT->DrawLine(D2D1::Point2F(cx-5.0f, cy), D2D1::Point2F(cx+5.0f, cy), pTextBrush, 1.5f);
                    } else if (id == 2) { // Sabitleme (Pin)
                        pMenuRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy-2.0f), 2.5f, 2.5f), pTextBrush, 1.5f);
                        pMenuRT->DrawLine(D2D1::Point2F(cx, cy+0.5f), D2D1::Point2F(cx, cy+6.0f), pTextBrush, 1.5f);
                    } else if (id == 3) { // Görevi Sonlandır (Yasaklama)
                        pMenuRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5.0f, 5.0f), pTextBrush, 1.5f);
                        pMenuRT->DrawLine(D2D1::Point2F(cx-3.5f, cy-3.5f), D2D1::Point2F(cx+3.5f, cy+3.5f), pTextBrush, 1.5f);
                    } else if (id == 4) { // Kapat (Çarpı)
                        pMenuRT->DrawLine(D2D1::Point2F(cx-4.0f, cy-4.0f), D2D1::Point2F(cx+4.0f, cy+4.0f), pTextBrush, 1.5f);
                        pMenuRT->DrawLine(D2D1::Point2F(cx-4.0f, cy+4.0f), D2D1::Point2F(cx+4.0f, cy-4.0f), pTextBrush, 1.5f);
                    }
                    
                    // Metin
                    if (pTextFormat) {
                        pMenuRT->DrawText(g_menuItems[i].text.c_str(), g_menuItems[i].text.length(), pTextFormat, 
                                          D2D1::RectF(40, currentY, size.width - 10, currentY + itemHeight), pTextBrush);
                    }
                    
                    currentY += itemHeight;
                }

                pBgBrush->Release(); pBorderBrush->Release(); pTextBrush->Release(); pHoverBrush->Release(); 
                pMenuRT->EndDraw();
            }
            EndPaint(hwnd, &ps); 
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt; pt.x = GET_X_LPARAM(lParam); pt.y = GET_Y_LPARAM(lParam);
            int hovered = -1; float currentY = 8.0f; 
            for (size_t i = 0; i < g_menuItems.size(); i++) {
                if (pt.y >= currentY && pt.y <= currentY + 36.0f) { hovered = i; break; }
                currentY += 36.0f;
            }
            if (g_hoveredMenuItem != hovered) { g_hoveredMenuItem = hovered; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (g_hoveredMenuItem >= 0 && g_hoveredMenuItem < g_menuItems.size()) {
                int cmdId = g_menuItems[g_hoveredMenuItem].id;
                
                if (cmdId == 1) { 
                    ShellExecute(NULL, L"open", L"ms-settings:", NULL, NULL, SW_SHOWNORMAL);
                } else if (cmdId == 2) { 
                    if (g_menuAppIsPinned) {
                        for (auto it = g_pinnedApps.begin(); it != g_pinnedApps.end(); ++it) {
                            if (it->first == g_menuAppId) { g_pinnedApps.erase(it); break; }
                        }
                    } else g_pinnedApps.push_back({g_menuAppId, g_menuAppIsUWP});
                    SavePinnedApps(); RefreshOpenApps();
                } else if (cmdId == 3) {
                    if (g_menuAppHwnd) {
                        DWORD pid = 0; GetWindowThreadProcessId(g_menuAppHwnd, &pid);
                        if (pid != 0) {
                            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                            if (hProc) { TerminateProcess(hProc, 0); CloseHandle(hProc); }
                        }
                    }
                } else if (cmdId == 4) {
                    if (g_menuAppHwnd) PostMessage(g_menuAppHwnd, WM_CLOSE, 0, 0);
                }
            }
            DestroyWindow(hwnd); return 0;
        }
        case WM_ACTIVATE: if (LOWORD(wParam) == WA_INACTIVE) DestroyWindow(hwnd); return 0;
        case WM_DESTROY: if (pMenuRT) { pMenuRT->Release(); pMenuRT = nullptr; } g_hMenu = NULL; return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: InitD2D(hwnd); SetTimer(hwnd, 1, 150, NULL); return 0;
        case WM_TIMER: if (wParam == 1 && !g_hMenu) RefreshOpenApps(); return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE; 
        
        case WM_NCHITTEST: {
            POINT pt; pt.x = GET_X_LPARAM(lParam); pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd, &pt);
            float barLeft = (GetSystemMetrics(SM_CXSCREEN) - g_dockRect.right) / 2.0f; 
            if (pt.y >= g_dockRect.top && pt.x >= barLeft && pt.x <= (GetSystemMetrics(SM_CXSCREEN) - barLeft)) {
                return HTCLIENT;
            }
            return HTTRANSPARENT; 
        }

        case WM_RBUTTONDOWN: {
            POINT pt; GetCursorPos(&pt); POINT clientPt = pt; ScreenToClient(hwnd, &clientPt); float mouseX = clientPt.x;
            RECT rc; GetWindowRect(hwnd, &rc);

            for (auto& app : openApps) {
                if (std::abs(mouseX - app.xOffset) < (app.currentSize / 2.0f)) {
                    if (app.hwnd != (HWND)1) {
                        if (g_hMenu) DestroyWindow(g_hMenu);
                        
                        g_menuAppId = app.id;
                        g_menuAppHwnd = app.hwnd;
                        g_menuAppIsPinned = app.isPinned;
                        g_menuAppIsUWP = app.isUWP;
                        g_hoveredMenuItem = -1;
                        
                        g_menuItems.clear();
                        g_menuItems.push_back({1, L"Ayarlar"});
                        if (g_menuAppIsPinned) g_menuItems.push_back({2, L"Görev çubuğundan kaldır"});
                        else g_menuItems.push_back({2, L"Görev çubuğuna sabitle"});
                        
                        if (g_menuAppHwnd) {
                            g_menuItems.push_back({3, L"Görevi sonlandır"}); 
                            g_menuItems.push_back({4, L"Pencereyi kapat"});  
                        }
                        
                        // 🪄 YENİ: Tam Mutlak Konumlandırma (Aşırı yukarıda açılmayı KÖKÜNDEN çözer)
                        int menuWidth = 230;
                        int menuHeight = g_menuItems.size() * 36 + 16;
                        int menuX = static_cast<int>((rc.left + app.xOffset) - (menuWidth / 2.0f));
                        
                        int sh = GetSystemMetrics(SM_CYSCREEN);
                        int menuY = sh - 62 - menuHeight; // Ekranın mutlak alt çizgisinden tam olarak menü + 8 piksel yukarıya hizalar.
                        
                        g_hMenu = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"MacDockMenu", L"", WS_POPUP, menuX, menuY, menuWidth, menuHeight, NULL, NULL, GetModuleHandle(NULL), NULL);
                        SetLayeredWindowAttributes(g_hMenu, 0, 255, LWA_ALPHA);
                        MARGINS margins = {-1, -1, -1, -1}; DwmExtendFrameIntoClientArea(g_hMenu, &margins);
                        DWORD corners = 2; DwmSetWindowAttribute(g_hMenu, 33, &corners, sizeof(corners)); 
                        ShowWindow(g_hMenu, SW_SHOW); SetForegroundWindow(g_hMenu); 
                    } break;
                }
            } return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt; GetCursorPos(&pt); POINT clientPt = pt; ScreenToClient(hwnd, &clientPt); float mouseX = clientPt.x;
            
            for (auto& app : openApps) {
                if (std::abs(mouseX - app.xOffset) < (app.currentSize / 2.0f)) {
                    if (app.hwnd == (HWND)1) {
                        keybd_event(VK_LWIN, 0, 0, 0); keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
                    } else if (app.hwnd == NULL) {
                        if (app.isUWP) { std::wstring launchStr = L"shell:AppsFolder\\" + app.id; ShellExecute(NULL, L"open", launchStr.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        } else ShellExecute(NULL, L"open", app.id.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    } else {
                        HWND fg = GetForegroundWindow(); HWND rootFg = GetAncestor(fg, GA_ROOTOWNER);
                        if (fg == app.hwnd || rootFg == app.hwnd) {
                            ShowWindow(app.hwnd, SW_MINIMIZE); PostMessage(app.hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                        } else {
                            if (IsIconic(app.hwnd)) ShowWindow(app.hwnd, SW_RESTORE); SetForegroundWindow(app.hwnd);
                        }
                    } break;
                }
            } return 0;
        }
            
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            for(auto& app : openApps) if(app.pBitmap) app.pBitmap->Release();
            if (pRenderTarget) pRenderTarget->Release(); if (pFactory) pFactory->Release(); if (pWicFactory) pWicFactory->Release();
            if (pTextFormat) pTextFormat->Release(); if (pDWriteFactory) pDWriteFactory->Release();
            CoUninitialize(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"MacDockMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBox(NULL, L"Dock zaten arka planda çalışıyor!", L"Bilgi", MB_OK | MB_ICONWARNING);
        return 0;
    }

    TaskbarGuard guard; 
    WNDCLASS wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"MacDock"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClass(&wc);
    WNDCLASS mc = {0}; mc.lpfnWndProc = MenuWndProc; mc.hInstance = hInst; mc.lpszClassName = L"MacDockMenu"; mc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClass(&mc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    
    g_hDock = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, 
        wc.lpszClassName, L"Dock", WS_POPUP, 
        0, sh - 100, sw, 100, 
        NULL, NULL, hInst, NULL
    );

    SetLayeredWindowAttributes(g_hDock, 0, 255, LWA_ALPHA);
    MARGINS margins = {-1, -1, -1, -1}; DwmExtendFrameIntoClientArea(g_hDock, &margins);

    ShowWindow(g_hDock, SW_SHOW);
    MSG msg;
    
    while (true) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break; TranslateMessage(&msg); DispatchMessage(&msg);
        } else { 
            ShowWindow(g_hDock, SW_SHOWNA); 
            Render(); 
            DwmFlush(); 
        }
    }
    
    ReleaseMutex(hMutex); CloseHandle(hMutex);
    return 0;
}