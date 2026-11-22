// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "MenuIcons.h"

#include "Icons.h"

#include <wincodec.h>

#include <map>

namespace menuicons {

namespace {

std::map<std::wstring, HBITMAP> g_cache; // by icon file (nullptr = can't decode)

template<typename T> void release(T*& pointer)
{
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

HBITMAP decode(const std::wstring& path, int size)
{
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return nullptr;
    HBITMAP bitmap = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT frames = 0;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(decoder->GetFrameCount(&frames)) && frames > 0) {
        // .ico files hold several sizes: take the smallest one at least
        // `size` wide, else the largest.
        UINT best = 0, bestWidth = 0;
        for (UINT i = 0; i < frames && i < 32; i++) {
            IWICBitmapFrameDecode* candidate = nullptr;
            UINT width = 0, height = 0;
            if (SUCCEEDED(decoder->GetFrame(i, &candidate)) && SUCCEEDED(candidate->GetSize(&width, &height))) {
                bool better = bestWidth == 0
                    || (width >= static_cast<UINT>(size) && (bestWidth < static_cast<UINT>(size) || width < bestWidth))
                    || (bestWidth < static_cast<UINT>(size) && width > bestWidth);
                if (better) {
                    best = i;
                    bestWidth = width;
                }
            }
            release(candidate);
        }
        if (SUCCEEDED(decoder->GetFrame(best, &frame))
            && SUCCEEDED(factory->CreateBitmapScaler(&scaler))
            && SUCCEEDED(scaler->Initialize(frame, size, size, WICBitmapInterpolationModeFant))
            && SUCCEEDED(factory->CreateFormatConverter(&converter))
            && SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            BITMAPINFO info = { };
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = size;
            info.bmiHeader.biHeight = -size; // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC screen = GetDC(nullptr);
            bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, screen);
            if (bitmap && FAILED(converter->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(bits)))) {
                DeleteObject(bitmap);
                bitmap = nullptr;
            }
        }
    }
    release(converter);
    release(scaler);
    release(frame);
    release(decoder);
    release(factory);
    return bitmap;
}

} // namespace

HBITMAP forPage(const std::wstring& pageURL)
{
    std::wstring path = icons::filePathFor(pageURL);
    if (path.empty() || path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".svg") == 0)
        return nullptr;
    auto found = g_cache.find(path);
    if (found != g_cache.end())
        return found->second;
    HBITMAP bitmap = decode(path, GetSystemMetrics(SM_CXSMICON));
    g_cache[path] = bitmap;
    return bitmap;
}

void apply(HMENU menu, UINT commandId, const std::wstring& pageURL)
{
    HBITMAP bitmap = forPage(pageURL);
    if (!bitmap)
        return;
    MENUITEMINFOW item = { };
    item.cbSize = sizeof(item);
    item.fMask = MIIM_BITMAP;
    item.hbmpItem = bitmap;
    SetMenuItemInfoW(menu, commandId, FALSE, &item);
}

} // namespace menuicons
