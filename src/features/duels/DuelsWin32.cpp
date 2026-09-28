#include "DuelsWin32.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace Duels
{
    // The largest visible top-level window of this process; Hyperspace may create small helper windows.
    static BOOL CALLBACK FindGameWindow(HWND window, LPARAM param)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL)
        {
            return TRUE;
        }

        RECT rect;
        GetWindowRect(window, &rect);
        long area = (long)(rect.right - rect.left) * (rect.bottom - rect.top);

        HWND *best = reinterpret_cast<HWND*>(param);
        if (*best == NULL)
        {
            *best = window;
        }
        else
        {
            RECT bestRect;
            GetWindowRect(*best, &bestRect);
            if (area > (long)(bestRect.right - bestRect.left) * (bestRect.bottom - bestRect.top)) *best = window;
        }
        return TRUE;
    }

    static HWND GameWindow()
    {
        HWND found = NULL;
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
        return found;
    }

    static std::string Describe(HWND window)
    {
        char title[128] = {0};
        char className[64] = {0};
        GetWindowTextA(window, title, sizeof(title));
        GetClassNameA(window, className, sizeof(className));
        char buffer[320];
        snprintf(buffer, sizeof(buffer), "window '%s' (class %s) iconic=%d foreground=%d", title, className,
                 IsIconic(window) ? 1 : 0, GetForegroundWindow() == window ? 1 : 0);
        return buffer;
    }

    bool MinimizeGameWindow(std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        ShowWindow(window, SW_MINIMIZE);
        details = Describe(window);
        return true;
    }

    bool RestoreGameWindow(std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        details = Describe(window);
        return true;
    }

    bool MoveGameWindow(int x, int y, std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        details = Describe(window);
        return true;
    }

    bool CaptureGameWindow(const std::string &path, std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        RECT client;
        GetClientRect(window, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        if (width <= 0 || height <= 0)
        {
            details = "the window has no area (minimized?)";
            return true;
        }

        HDC screen = GetDC(NULL);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info;
        ZeroMemory(&info, sizeof(info));
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = height;   // bottom-up, as BMP files store it
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = NULL;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        // FTL draws through Direct3D here, which PrintWindow can't read (black). Copy the window's area from the
        // composited screen instead; that needs the window to be visible, e.g. two games side by side.
        POINT origin = {0, 0};
        ClientToScreen(window, &origin);
        BOOL printed = BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
        SelectObject(memory, previous);

        bool saved = false;
        if (printed && pixels)
        {
            int stride = ((width * 3 + 3) / 4) * 4;
            BITMAPFILEHEADER file;
            ZeroMemory(&file, sizeof(file));
            file.bfType = 0x4D42;   // "BM"
            file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            file.bfSize = file.bfOffBits + stride * height;
            FILE *out = fopen(path.c_str(), "wb");
            if (out)
            {
                fwrite(&file, sizeof(file), 1, out);
                fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, out);
                fwrite(pixels, stride * height, 1, out);
                fclose(out);
                saved = true;
            }
        }
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(NULL, screen);

        char buffer[160];
        snprintf(buffer, sizeof(buffer), "%dx%d %s", width, height, saved ? "saved" : (printed ? "could not write the file" : "screen copy failed"));
        details = buffer;
        return true;
    }
}

#else

namespace Duels
{
    bool MinimizeGameWindow(std::string &details) { details = "not supported"; return false; }
    bool RestoreGameWindow(std::string &details) { details = "not supported"; return false; }
    bool MoveGameWindow(int, int, std::string &details) { details = "not supported"; return false; }
    bool CaptureGameWindow(const std::string &, std::string &details) { details = "not supported"; return false; }
}

#endif
