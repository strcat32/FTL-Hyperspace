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
}

#else

namespace Duels
{
    bool MinimizeGameWindow(std::string &details) { details = "not supported"; return false; }
    bool RestoreGameWindow(std::string &details) { details = "not supported"; return false; }
}

#endif
