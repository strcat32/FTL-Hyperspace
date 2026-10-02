#include "DuelsWin32.h"

#include <cstdio>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>

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

    bool SetGameWindowTitle(const char *title, std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        SetWindowTextA(window, title);
        details = Describe(window);
        return true;
    }

    bool SetGameWindowIcon(const char *file, std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        HICON big = (HICON)LoadImageA(NULL, file, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
        HICON small = (HICON)LoadImageA(NULL, file, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
        if (!big && !small)
        {
            details = std::string("no icon in ") + file;
            return false;
        }
        if (big) SendMessageA(window, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (small) SendMessageA(window, WM_SETICON, ICON_SMALL, (LPARAM)small);
        details = Describe(window);
        return true;
    }

    bool PutFileInPlace(const std::string &from, const std::string &to)
    {
        return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    bool MoveGameWindow(int x, int y, int width, int height, std::string &details)
    {
        HWND window = GameWindow();
        if (!window) return false;
        if (width > 0 && height > 0)
        {
            // The size given is the drawing area; add the frame around it.
            RECT rect = {0, 0, width, height};
            AdjustWindowRectEx(&rect, (DWORD)GetWindowLongPtr(window, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtr(window, GWL_EXSTYLE));
            SetWindowPos(window, NULL, x, y, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        details = Describe(window);
        return true;
    }

    bool MapToTestDisplay(int &x, int &y, int &width, int &height)
    {
        const char *display = getenv("DUELS_DISPLAY");
        int left, top, displayWidth, displayHeight;
        if (!display || sscanf(display, "%d,%d,%d,%d", &left, &top, &displayWidth, &displayHeight) != 4) return false;
        double scale = 1.0;
        if (const char *text = getenv("DUELS_DISPLAY_SCALE")) scale = atof(text);
        if (!(scale > 0.1 && scale <= 1.0)) scale = 1.0;
        if ((width <= 0 || height <= 0) && scale < 1.0)
        {
            width = 1280;
            height = 720;
        }
        x = left + (int)(x * scale);
        y = top + (int)(y * scale);
        if (width > 0 && height > 0)
        {
            width = (int)(width * scale);
            height = (int)(height * scale);
        }
        return true;
    }

    void PlaceOnTestDisplay()
    {
        static bool placed = false;
        if (placed) return;
        const char *position = getenv("DUELS_WINDOW");
        int x, y;
        if (!position || sscanf(position, "%d,%d", &x, &y) != 2)
        {
            placed = true;   // not asked for
            return;
        }
        int width = 0, height = 0;
        std::string details;
        if (!MapToTestDisplay(x, y, width, height)) return;
        placed = MoveGameWindow(x, y, width, height, details);
    }
}

#else

namespace Duels
{
    bool MinimizeGameWindow(std::string &details) { details = "not supported"; return false; }
    bool RestoreGameWindow(std::string &details) { details = "not supported"; return false; }
    bool MoveGameWindow(int, int, int, int, std::string &details) { details = "not supported"; return false; }
    bool MapToTestDisplay(int &, int &, int &, int &) { return false; }
    void PlaceOnTestDisplay() {}
    bool SetGameWindowTitle(const char *, std::string &details) { details = "not supported"; return false; }
    bool SetGameWindowIcon(const char *, std::string &details) { details = "not supported"; return false; }
    bool PutFileInPlace(const std::string &from, const std::string &to) { return std::rename(from.c_str(), to.c_str()) == 0; }
}

#endif
