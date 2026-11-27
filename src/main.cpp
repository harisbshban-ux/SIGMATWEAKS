#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cwchar>

namespace {

constexpr wchar_t kWindowClass[] = L"SigmaTweaksWindow";
constexpr UINT kCleanupComplete = WM_APP + 1;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr int kMinimumSpeed = 1;
constexpr int kMaximumSpeed = 20;

const COLORREF kBackground = RGB(15, 18, 30);
const COLORREF kPanel = RGB(25, 30, 46);
const COLORREF kPanelRaised = RGB(32, 39, 58);
const COLORREF kBorder = RGB(48, 57, 78);
const COLORREF kText = RGB(239, 242, 250);
const COLORREF kMuted = RGB(157, 168, 190);
const COLORREF kCyan = RGB(91, 220, 239);
const COLORREF kViolet = RGB(166, 132, 255);
const COLORREF kLime = RGB(183, 238, 118);
const COLORREF kAmber = RGB(255, 197, 106);

struct CleanupTarget {
    std::wstring name;
    std::wstring path;
    std::wstring reason;
};

struct CleanupResult {
    std::size_t removed = 0;
    std::size_t skipped = 0;
    std::size_t errors = 0;
    DWORD firstError = ERROR_SUCCESS;
};

enum class FocusItem : int {
    Slider,
    ApplySpeed,
    Animations,
    UserTemp,
    WindowsTemp,
    Prefetch,
    Clean,
    Count
};

enum class HoverItem {
    None,
    ApplySpeed,
    Animations,
    Clean
};

struct Layout {
    RECT speedCard{};
    RECT performanceCard{};
    RECT cleanupCard{};
    RECT slider{};
    RECT applySpeed{};
    RECT animations{};
    std::array<RECT, 3> cleanupOptions{};
    RECT clean{};
};

struct AppState {
    int speed = 10;
    bool clientAnimations = true;
    bool menuAnimations = true;
    bool animationsAvailable = false;
    std::array<bool, 3> selectedCleanup{{true, true, false}};
    bool cleaning = false;
    bool draggingSlider = false;
    bool mouseDown = false;
    FocusItem pressedItem = FocusItem::Slider;
    FocusItem focus = FocusItem::Slider;
    HoverItem hover = HoverItem::None;
    float hoverMix = 0.0f;
    float animationPhase = 0.0f;
    UINT dpi = 96;
    HFONT titleFont = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    Layout layout{};
    std::array<CleanupTarget, 3> cleanupTargets{};
    std::wstring status = L"Ready. Changes apply only after you confirm them.";
};

RECT MakeRect(int left, int top, int right, int bottom) {
    return RECT{left, top, right, bottom};
}

bool Contains(const RECT& rect, POINT point) {
    return PtInRect(&rect, point) != FALSE;
}

COLORREF MixColor(COLORREF from, COLORREF to, float amount) {
    amount = std::max(0.0f, std::min(amount, 1.0f));
    const auto mix = [amount](int a, int b) {
        return static_cast<BYTE>(a + (b - a) * amount);
    };
    return RGB(mix(GetRValue(from), GetRValue(to)),
               mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

void FillRounded(HDC dc, const RECT& rect, COLORREF fill, COLORREF outline, int radius = 14) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void FillCircle(HDC dc, int x, int y, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - radius, y - radius, x + radius, y + radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void DrawText(HDC dc, const std::wstring& text, RECT rect, HFONT font,
              COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

HFONT CreateUiFont(int points, int weight, UINT dpi) {
    return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

void CreateFonts(AppState& state) {
    state.dpi = 96;
    state.titleFont = CreateUiFont(21, FW_BOLD, state.dpi);
    state.headingFont = CreateUiFont(15, FW_SEMIBOLD, state.dpi);
    state.bodyFont = CreateUiFont(10, FW_NORMAL, state.dpi);
    state.smallFont = CreateUiFont(9, FW_NORMAL, state.dpi);
}

void DestroyFonts(AppState& state) {
    for (HFONT font : {state.titleFont, state.headingFont, state.bodyFont, state.smallFont}) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
}

bool ReadPointerSpeed(int& speed) {
    int current = 0;
    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &current, 0)) {
        return false;
    }
    speed = std::max(kMinimumSpeed, std::min(current, kMaximumSpeed));
    return true;
}

bool ReadAnimationSettings(bool& client, bool& menu) {
    BOOL clientValue = TRUE;
    BOOL menuValue = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientValue, 0) ||
        !SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &menuValue, 0)) {
        return false;
    }
    client = clientValue != FALSE;
    menu = menuValue != FALSE;
    return true;
}

std::wstring GetEnvironmentPath(const wchar_t* name) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::wstring GetWindowsPath() {
    std::array<wchar_t, 32768> buffer{};
    const UINT length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring LongPath(const std::wstring& path);

std::wstring NormalizeLocalDirectory(const std::wstring& input, const wchar_t* expectedName,
                                     std::wstring& reason) {
    if (input.size() < 3 || input[1] != L':' ||
        (input[2] != L'\\' && input[2] != L'/')) {
        reason = L"Not an absolute local drive path";
        return {};
    }

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()),
                                           buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) {
        reason = L"Could not resolve the folder path";
        return {};
    }
    std::wstring fullPath(buffer.data(), length);
    std::replace(fullPath.begin(), fullPath.end(), L'/', L'\\');
    while (fullPath.size() > 3 && (fullPath.back() == L'\\' || fullPath.back() == L'/')) {
        fullPath.pop_back();
    }
    wchar_t driveRoot[] = {fullPath[0], L':', L'\\', L'\0'};
    if (GetDriveTypeW(driveRoot) == DRIVE_REMOTE) {
        reason = L"Network folders are not supported";
        return {};
    }

    const std::size_t separator = fullPath.find_last_of(L"\\/");
    const std::wstring leaf = separator == std::wstring::npos ? fullPath : fullPath.substr(separator + 1);
    if (!EqualsIgnoreCase(leaf, expectedName)) {
        reason = L"Folder name does not match the selected cleanup target";
        return {};
    }

    const DWORD attributes = GetFileAttributesW(LongPath(fullPath).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        reason = L"Folder is unavailable";
        return {};
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        reason = L"Folder is a link or mount point; it was not followed";
        return {};
    }
    return fullPath;
}

std::array<CleanupTarget, 3> ResolveCleanupTargets() {
    std::array<CleanupTarget, 3> targets{{
        {L"User temporary files", {}, {}},
        {L"Windows temporary files", {}, {}},
        {L"Prefetch cache", {}, {}}
    }};

    const std::wstring userTemp = GetEnvironmentPath(L"TEMP");
    targets[0].path = NormalizeLocalDirectory(userTemp, L"Temp", targets[0].reason);

    const std::wstring windows = GetWindowsPath();
    if (windows.empty()) {
        targets[1].reason = L"Could not resolve the Windows folder";
        targets[2].reason = targets[1].reason;
    } else {
        targets[1].path = NormalizeLocalDirectory(windows + L"\\Temp", L"Temp",
                                                   targets[1].reason);
        targets[2].path = NormalizeLocalDirectory(windows + L"\\Prefetch", L"Prefetch",
                                                   targets[2].reason);
    }
    return targets;
}

std::wstring LongPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    return L"\\\\?\\" + path;
}

bool IsSkippableError(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
           error == ERROR_LOCK_VIOLATION || error == ERROR_DIR_NOT_EMPTY;
}

void RecordFailure(CleanupResult& result, DWORD error) {
    if (IsSkippableError(error)) {
        ++result.skipped;
    } else {
        ++result.errors;
        if (result.firstError == ERROR_SUCCESS) {
            result.firstError = error;
        }
    }
}

struct DirectoryWork {
    std::wstring path;
    bool removeAfter = false;
};

void RemoveDirectoryContents(const std::wstring& root, CleanupResult& result) {
    std::vector<DirectoryWork> pending;
    pending.push_back({root, false});

    while (!pending.empty()) {
        DirectoryWork current = std::move(pending.back());
        pending.pop_back();
        const std::wstring apiPath = LongPath(current.path);
        const DWORD attributes = GetFileAttributesW(apiPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            RecordFailure(result, GetLastError());
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            RecordFailure(result, ERROR_DIRECTORY);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ++result.skipped;
            continue;
        }

        if (current.removeAfter) {
            if (RemoveDirectoryW(apiPath.c_str())) {
                ++result.removed;
            } else {
                RecordFailure(result, GetLastError());
            }
            continue;
        }

        std::vector<DirectoryWork> childDirectories;
        const std::wstring searchPath = LongPath(current.path + L"\\*");
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW(searchPath.c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) {
                RecordFailure(result, error);
            }
            continue;
        }

        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring child = current.path + L"\\" + entry.cFileName;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                ++result.skipped;
            } else if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                childDirectories.push_back({child, false});
            } else if (DeleteFileW(LongPath(child).c_str())) {
                ++result.removed;
            } else {
                RecordFailure(result, GetLastError());
            }
        } while (FindNextFileW(search, &entry));

        const DWORD enumerationError = GetLastError();
        FindClose(search);
        if (enumerationError != ERROR_NO_MORE_FILES) {
            RecordFailure(result, enumerationError);
        }

        for (auto it = childDirectories.rbegin(); it != childDirectories.rend(); ++it) {
            pending.push_back({it->path, true});
            pending.push_back({it->path, false});
        }
    }
}

void CleanSelectedTargets(const std::array<CleanupTarget, 3>& targets,
                          const std::array<bool, 3>& selected, CleanupResult& result) {
    std::vector<std::wstring> alreadyCleaned;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!selected[i]) {
            continue;
        }
        const CleanupTarget& target = targets[i];
        if (target.path.empty()) {
            ++result.errors;
            if (result.firstError == ERROR_SUCCESS) {
                result.firstError = ERROR_PATH_NOT_FOUND;
            }
            continue;
        }
        const DWORD attributes = GetFileAttributesW(LongPath(target.path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            RecordFailure(result, GetLastError());
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            RecordFailure(result, ERROR_DIRECTORY);
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ++result.skipped;
            continue;
        }
        const bool duplicate = std::any_of(alreadyCleaned.begin(), alreadyCleaned.end(),
                                           [&target](const std::wstring& path) {
                                               return EqualsIgnoreCase(path, target.path);
                                           });
        if (duplicate) {
            continue;
        }
        alreadyCleaned.push_back(target.path);
        RemoveDirectoryContents(target.path, result);
    }
}

void RebuildLayout(AppState& state, int width, int height) {
    const int margin = 30;
    const int gap = 18;
    const int top = 104;
    const int cardHeight = 286;
    const int availableWidth = std::max(100, width - margin * 2 - gap);
    const int leftWidth = availableWidth / 2;
    const int rightX = margin + leftWidth + gap;
    const int rightWidth = std::max(100, width - margin - rightX);
    state.layout.speedCard = MakeRect(margin, top, margin + leftWidth, top + cardHeight);
    state.layout.performanceCard = MakeRect(rightX, top, rightX + rightWidth, top + cardHeight);

    const int cleanupTop = top + cardHeight + gap;
    const int cleanupBottom = std::max(cleanupTop + 225, height - 48);
    state.layout.cleanupCard = MakeRect(margin, cleanupTop, width - margin, cleanupBottom);

    const RECT speed = state.layout.speedCard;
    state.layout.slider = MakeRect(speed.left + 24, speed.top + 153, speed.right - 24, speed.top + 177);
    state.layout.applySpeed = MakeRect(speed.left + 24, speed.top + 218,
                                       speed.left + 194, speed.top + 263);

    const RECT performance = state.layout.performanceCard;
    state.layout.animations = MakeRect(performance.left + 24, performance.top + 218,
                                       performance.left + 230, performance.top + 263);

    const RECT cleanup = state.layout.cleanupCard;
    const int optionGap = 11;
    const int optionWidth = std::max(100, (cleanup.right - cleanup.left - 48 - optionGap * 2) / 3);
    for (std::size_t i = 0; i < state.layout.cleanupOptions.size(); ++i) {
        const int x = cleanup.left + 24 + static_cast<int>(i) * (optionWidth + optionGap);
        state.layout.cleanupOptions[i] = MakeRect(x, cleanup.top + 91, x + optionWidth,
                                                  cleanup.top + 179);
    }
    state.layout.clean = MakeRect(cleanup.left + 24, cleanup.top + 197,
                                  cleanup.left + 225, cleanup.top + 242);
}

void DrawCard(HDC dc, const RECT& rect) {
    FillRounded(dc, rect, kPanel, kBorder, 17);
}

void DrawPill(HDC dc, RECT rect, const std::wstring& label, COLORREF fill, COLORREF text,
              HFONT font) {
    FillRounded(dc, rect, fill, fill, 16);
    DrawText(dc, label, rect, font, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawButton(HDC dc, const RECT& rect, const std::wstring& label, HFONT font,
                COLORREF accent, float hover, bool enabled, bool focused) {
    const COLORREF normalFill = enabled ? MixColor(kPanelRaised, accent, 0.12f) : kPanelRaised;
    const COLORREF hoverFill = enabled ? MixColor(normalFill, accent, 0.62f * hover) : kPanelRaised;
    const COLORREF outline = focused ? accent : MixColor(kBorder, accent, hover * 0.5f);
    FillRounded(dc, rect, hoverFill, outline, 12);
    RECT textRect = rect;
    DrawText(dc, label, textRect, font, enabled ? kText : kMuted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (focused) {
        RECT inner = {rect.left + 4, rect.top + 4, rect.right - 4, rect.bottom - 4};
        HBRUSH brush = CreateSolidBrush(hoverFill);
        HPEN pen = CreatePen(PS_DOT, 1, accent);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, inner.left, inner.top, inner.right, inner.bottom, 9, 9);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
}

bool IsFocus(const AppState& state, FocusItem item) {
    return state.focus == item;
}

float ButtonHover(const AppState& state, HoverItem item) {
    return state.hover == item ? state.hoverMix : 0.0f;
}

std::wstring AnimationStatus(const AppState& state) {
    if (!state.animationsAvailable) {
        return L"Animation settings are unavailable on this Windows version.";
    }
    if (!state.clientAnimations && !state.menuAnimations) {
        return L"Reduced: client-area and menu animations are off.";
    }
    if (state.clientAnimations && state.menuAnimations) {
        return L"Windows menu and client-area animations are on.";
    }
    return L"Mixed: one animation setting is currently disabled.";
}

void DrawWindow(HDC dc, AppState& state, int width, int height) {
    RebuildLayout(state, width, height);
    RECT client = MakeRect(0, 0, width, height);
    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);

    FillCircle(dc, width - 58, 46, 5,
               MixColor(kCyan, kViolet, (std::sin(state.animationPhase) + 1.0f) * 0.5f));
    RECT brand = MakeRect(32, 26, 270, 61);
    DrawText(dc, L"SIGMA", brand, state.titleFont, kText);
    RECT brandAccent = MakeRect(130, 26, 265, 61);
    DrawText(dc, L"// TWEAKS", brandAccent, state.headingFont, kCyan);
    RECT tagline = MakeRect(273, 28, width - 120, 59);
    DrawText(dc, L"A considered toolkit for everyday Windows tuning", tagline,
             state.smallFont, kMuted);
    RECT badge = MakeRect(width - 111, 28, width - 42, 56);
    DrawPill(dc, badge, L"LOCAL", RGB(31, 51, 53), kLime, state.smallFont);

    const RECT speed = state.layout.speedCard;
    DrawCard(dc, speed);
    DrawText(dc, L"Pointer speed", MakeRect(speed.left + 24, speed.top + 18,
                                             speed.right - 20, speed.top + 48),
             state.headingFont, kText);
    DrawText(dc, L"Adjust the Windows mouse pointer speed for this account.",
             MakeRect(speed.left + 24, speed.top + 51, speed.right - 20, speed.top + 75),
             state.smallFont, kMuted);
    DrawText(dc, L"Speed", MakeRect(speed.left + 24, speed.top + 101,
                                    speed.left + 105, speed.top + 130),
             state.bodyFont, kMuted);
    const std::wstring speedText = std::to_wstring(state.speed) + L" / 20";
    DrawText(dc, speedText, MakeRect(speed.right - 106, speed.top + 92, speed.right - 24,
                                     speed.top + 132),
             state.titleFont, kCyan, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const RECT slider = state.layout.slider;
    const int trackLeft = slider.left + 6;
    const int trackRight = slider.right - 6;
    const int trackY = (slider.top + slider.bottom) / 2;
    RECT track = MakeRect(trackLeft, trackY - 3, trackRight, trackY + 3);
    FillRounded(dc, track, kBorder, kBorder, 6);
    const float fraction = static_cast<float>(state.speed - kMinimumSpeed) /
                           static_cast<float>(kMaximumSpeed - kMinimumSpeed);
    const int knobX = trackLeft + static_cast<int>((trackRight - trackLeft) * fraction);
    RECT progress = MakeRect(trackLeft, trackY - 3, knobX, trackY + 3);
    if (progress.right > progress.left) {
        FillRounded(dc, progress, kCyan, kCyan, 6);
    }
    FillCircle(dc, knobX, trackY, 10, RGB(68, 206, 232));
    FillCircle(dc, knobX, trackY, 5, kText);
    if (IsFocus(state, FocusItem::Slider)) {
        FillCircle(dc, knobX, trackY, 14, MixColor(kCyan, kPanel, 0.55f));
        FillCircle(dc, knobX, trackY, 8, kCyan);
        FillCircle(dc, knobX, trackY, 4, kText);
    }
    DrawText(dc, L"Slower", MakeRect(slider.left, slider.bottom + 5, slider.left + 70,
                                     slider.bottom + 25), state.smallFont, kMuted);
    DrawText(dc, L"Faster", MakeRect(slider.right - 70, slider.bottom + 5, slider.right,
                                     slider.bottom + 25), state.smallFont, kMuted,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawButton(dc, state.layout.applySpeed, L"Apply speed", state.bodyFont, kCyan,
               ButtonHover(state, HoverItem::ApplySpeed), true,
               IsFocus(state, FocusItem::ApplySpeed));
    RECT speedNote = MakeRect(speed.left + 211, speed.top + 218, speed.right - 20, speed.top + 263);
    DrawText(dc, L"Windows setting  ·  1–20", speedNote, state.smallFont, kMuted);

    const RECT performance = state.layout.performanceCard;
    DrawCard(dc, performance);
    DrawText(dc, L"Performance options", MakeRect(performance.left + 24, performance.top + 18,
                                                  performance.right - 20, performance.top + 48),
             state.headingFont, kText);
    DrawText(dc, L"A small, reversible change to Windows UI effects.",
             MakeRect(performance.left + 24, performance.top + 51,
                      performance.right - 20, performance.top + 75),
             state.smallFont, kMuted);
    RECT info = MakeRect(performance.left + 24, performance.top + 93,
                         performance.right - 24, performance.top + 193);
    FillRounded(dc, info, RGB(29, 34, 51), RGB(40, 47, 67), 12);
    FillCircle(dc, info.left + 18, info.top + 24, 5, kViolet);
    DrawText(dc, L"Reduce UI animations", MakeRect(info.left + 33, info.top + 10,
                                                   info.right - 12, info.top + 35),
             state.bodyFont, kText);
    DrawText(dc, AnimationStatus(state), MakeRect(info.left + 16, info.top + 42,
                                                   info.right - 12, info.bottom - 9),
             state.smallFont, state.animationsAvailable ? kMuted : kAmber,
             DT_LEFT | DT_TOP | DT_WORDBREAK);
    const bool animationsEnabled = state.clientAnimations || state.menuAnimations;
    const std::wstring animationButton = !state.animationsAvailable
        ? L"Unavailable"
        : (animationsEnabled ? L"Reduce animations" : L"Restore animations");
    DrawButton(dc, state.layout.animations, animationButton, state.bodyFont, kViolet,
               ButtonHover(state, HoverItem::Animations), state.animationsAvailable,
               IsFocus(state, FocusItem::Animations));
    RECT performanceNote = MakeRect(performance.left + 243, performance.top + 218,
                                    performance.right - 18, performance.top + 263);
    DrawText(dc, L"No admin rights required", performanceNote, state.smallFont, kMuted);

    const RECT cleanup = state.layout.cleanupCard;
    DrawCard(dc, cleanup);
    DrawText(dc, L"Temporary file cleanup", MakeRect(cleanup.left + 24, cleanup.top + 16,
                                                      cleanup.right - 20, cleanup.top + 46),
             state.headingFont, kText);
    DrawText(dc, L"Choose exact folders. In-use items and links are skipped; selected folders stay in place.",
             MakeRect(cleanup.left + 24, cleanup.top + 48, cleanup.right - 20, cleanup.top + 70),
             state.smallFont, kMuted);

    const auto& targets = state.cleanupTargets;
    const std::array<std::wstring, 3> hints{{
        L"%TEMP%  ·  current account",
        L"Windows\\Temp",
        L"Windows\\Prefetch"
    }};
    const std::array<FocusItem, 3> focusItems{{
        FocusItem::UserTemp, FocusItem::WindowsTemp, FocusItem::Prefetch
    }};
    for (std::size_t i = 0; i < state.layout.cleanupOptions.size(); ++i) {
        const RECT option = state.layout.cleanupOptions[i];
        const COLORREF edge = IsFocus(state, focusItems[i]) ? kCyan : kBorder;
        FillRounded(dc, option, kPanelRaised, edge, 12);
        RECT checkbox = MakeRect(option.left + 12, option.top + 12,
                                 option.left + 31, option.top + 31);
        FillRounded(dc, checkbox, state.selectedCleanup[i] ? kCyan : kPanel,
                    state.selectedCleanup[i] ? kCyan : kMuted, 5);
        if (state.selectedCleanup[i]) {
            HPEN pen = CreatePen(PS_SOLID, 2, kBackground);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            MoveToEx(dc, checkbox.left + 4, checkbox.top + 10, nullptr);
            LineTo(dc, checkbox.left + 8, checkbox.top + 14);
            LineTo(dc, checkbox.left + 15, checkbox.top + 5);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        DrawText(dc, targets[i].name, MakeRect(option.left + 39, option.top + 8,
                                               option.right - 10, option.top + 35),
                 state.bodyFont, kText);
        DrawText(dc, hints[i], MakeRect(option.left + 12, option.top + 39,
                                        option.right - 10, option.top + 59),
                 state.smallFont, kMuted);
        const std::wstring pathLabel = targets[i].path.empty() ? targets[i].reason : targets[i].path;
        DrawText(dc, pathLabel, MakeRect(option.left + 12, option.top + 62,
                                         option.right - 10, option.bottom - 7),
                 state.smallFont, targets[i].path.empty() ? kAmber : kMuted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    DrawButton(dc, state.layout.clean,
               state.cleaning ? L"Cleaning..." : L"Clean selected",
               state.bodyFont, kLime, ButtonHover(state, HoverItem::Clean),
               !state.cleaning, IsFocus(state, FocusItem::Clean));
    RECT cleanupNote = MakeRect(state.layout.clean.right + 15, state.layout.clean.top,
                                cleanup.right - 20, state.layout.clean.bottom);
    DrawText(dc, state.cleaning ? L"Working without elevated privileges..."
                                : L"Selected folders only  ·  No elevation",
             cleanupNote, state.smallFont, state.cleaning ? kAmber : kMuted);

    RECT footer = MakeRect(32, height - 39, width - 32, height - 12);
    DrawText(dc, state.status, footer, state.smallFont, kMuted,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void UpdateHover(AppState& state, POINT point) {
    state.hover = HoverItem::None;
    if (Contains(state.layout.applySpeed, point)) {
        state.hover = HoverItem::ApplySpeed;
    } else if (Contains(state.layout.animations, point) && state.animationsAvailable) {
        state.hover = HoverItem::Animations;
    } else if (Contains(state.layout.clean, point) && !state.cleaning) {
        state.hover = HoverItem::Clean;
    }
}

void SetSpeedFromPoint(AppState& state, int x) {
    const RECT slider = state.layout.slider;
    const int left = slider.left + 6;
    const int right = slider.right - 6;
    if (right <= left) {
        return;
    }
    const float position = static_cast<float>(std::max(left, std::min(right, x)) - left) /
                           static_cast<float>(right - left);
    state.speed = kMinimumSpeed + static_cast<int>(
        position * (kMaximumSpeed - kMinimumSpeed) + 0.5f);
}

bool ApplyPointerSpeed(AppState& state, HWND window) {
    std::wstring confirmation = L"Set the Windows mouse pointer speed to " +
        std::to_wstring(state.speed) +
        L" out of 20? This updates the current Windows pointer setting.";
    if (MessageBoxW(window, confirmation.c_str(), L"Apply pointer speed",
                    MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return false;
    }
    if (!SystemParametersInfoW(SPI_SETMOUSESPEED, 0,
                               reinterpret_cast<PVOID>(static_cast<INT_PTR>(state.speed)),
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        const DWORD error = GetLastError();
        state.status = L"Could not change pointer speed (Windows error " +
                       std::to_wstring(error) + L").";
        MessageBoxW(window, state.status.c_str(), L"Pointer speed was not changed",
                    MB_ICONERROR | MB_OK);
        return false;
    }
    state.status = L"Pointer speed set to " + std::to_wstring(state.speed) + L" out of 20.";
    return true;
}

bool ApplyAnimationSettings(AppState& state, HWND window) {
    if (!state.animationsAvailable) {
        return false;
    }
    const bool reduce = state.clientAnimations || state.menuAnimations;
    BOOL desired = reduce ? FALSE : TRUE;
    const std::wstring choice = reduce ? L"Turn off Windows client-area and menu animations?"
                                       : L"Turn Windows client-area and menu animations back on?";
    if (MessageBoxW(window, choice.c_str(), L"Change UI animation settings",
                    MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return false;
    }
    const UINT flags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    if (!SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, &desired, flags)) {
        const DWORD error = GetLastError();
        state.status = L"Could not change client-area animations (Windows error " +
                       std::to_wstring(error) + L").";
        MessageBoxW(window, state.status.c_str(), L"Windows setting was not changed",
                    MB_ICONERROR | MB_OK);
        return false;
    }
    if (!SystemParametersInfoW(SPI_SETMENUANIMATION, 0, &desired, flags)) {
        const DWORD error = GetLastError();
        BOOL previous = state.clientAnimations ? TRUE : FALSE;
        const bool restored = SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0,
                                                     &previous, flags) != FALSE;
        state.animationsAvailable =
            ReadAnimationSettings(state.clientAnimations, state.menuAnimations);
        state.status = L"Could not change menu animations (Windows error " +
                       std::to_wstring(error) + L"). " +
                       (restored ? L"The previous client-area setting was restored."
                                 : L"Restoring the previous client-area setting also failed; check Windows Settings.");
        MessageBoxW(window, state.status.c_str(), L"Windows setting was not changed",
                    MB_ICONERROR | MB_OK);
        return false;
    }
    state.clientAnimations = desired != FALSE;
    state.menuAnimations = desired != FALSE;
    state.status = reduce
        ? L"Windows client-area and menu animations are now reduced; you can restore them here."
        : L"Windows client-area and menu animations have been restored.";
    return true;
}

std::wstring BuildCleanupConfirmation(const std::array<CleanupTarget, 3>& targets,
                                      const std::array<bool, 3>& selected) {
    std::wstring message = L"Remove files inside these selected folders? Their folders will remain.\n\n";
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!selected[i]) {
            continue;
        }
        message += L"• " + targets[i].name + L"\n  " +
                   (targets[i].path.empty() ? targets[i].reason : targets[i].path) + L"\n";
    }
    message += L"\nLocked, inaccessible, and linked items will be skipped. No administrator access will be requested.";
    return message;
}

void StartCleanup(AppState& state, HWND window) {
    if (state.cleaning) {
        return;
    }
    if (std::none_of(state.selectedCleanup.begin(), state.selectedCleanup.end(),
                     [](bool value) { return value; })) {
        state.status = L"Select at least one folder to clean.";
        return;
    }

    const auto targets = ResolveCleanupTargets();
    const std::wstring confirmation = BuildCleanupConfirmation(targets, state.selectedCleanup);
    if (MessageBoxW(window, confirmation.c_str(), L"Confirm temporary file cleanup",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    state.cleaning = true;
    state.status = L"Cleanup started. Windows may leave locked or inaccessible files in place.";
    const auto selected = state.selectedCleanup;
    auto* result = new (std::nothrow) CleanupResult;
    if (result == nullptr) {
        state.cleaning = false;
        state.status = L"Could not allocate cleanup state. No files were removed.";
        MessageBoxW(window, state.status.c_str(), L"Cleanup could not start", MB_ICONERROR | MB_OK);
        return;
    }
    try {
        std::thread([window, targets, selected, result]() {
            try {
                CleanSelectedTargets(targets, selected, *result);
            } catch (...) {
                ++result->errors;
                if (result->firstError == ERROR_SUCCESS) {
                    result->firstError = ERROR_NOT_ENOUGH_MEMORY;
                }
            }
            if (!PostMessageW(window, kCleanupComplete, 0, reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
        }).detach();
    } catch (const std::exception&) {
        delete result;
        state.cleaning = false;
        state.status = L"Could not start cleanup. No files were removed.";
        MessageBoxW(window, state.status.c_str(), L"Cleanup could not start", MB_ICONERROR | MB_OK);
    }
}

FocusItem FocusFromPoint(const AppState& state, POINT point) {
    if (Contains(state.layout.slider, point)) {
        return FocusItem::Slider;
    }
    if (Contains(state.layout.applySpeed, point)) {
        return FocusItem::ApplySpeed;
    }
    if (Contains(state.layout.animations, point)) {
        return FocusItem::Animations;
    }
    for (std::size_t i = 0; i < state.layout.cleanupOptions.size(); ++i) {
        if (Contains(state.layout.cleanupOptions[i], point)) {
            return static_cast<FocusItem>(static_cast<int>(FocusItem::UserTemp) +
                                          static_cast<int>(i));
        }
    }
    if (Contains(state.layout.clean, point)) {
        return FocusItem::Clean;
    }
    return state.focus;
}

void ActivateFocusedItem(AppState& state, HWND window) {
    switch (state.focus) {
    case FocusItem::Slider:
        state.speed = state.speed == kMaximumSpeed ? kMinimumSpeed : state.speed + 1;
        break;
    case FocusItem::ApplySpeed:
        ApplyPointerSpeed(state, window);
        break;
    case FocusItem::Animations:
        ApplyAnimationSettings(state, window);
        break;
    case FocusItem::UserTemp:
    case FocusItem::WindowsTemp:
    case FocusItem::Prefetch:
        state.selectedCleanup[static_cast<std::size_t>(
            static_cast<int>(state.focus) - static_cast<int>(FocusItem::UserTemp))] =
            !state.selectedCleanup[static_cast<std::size_t>(
                static_cast<int>(state.focus) - static_cast<int>(FocusItem::UserTemp))];
        break;
    case FocusItem::Clean:
        StartCleanup(state, window);
        break;
    case FocusItem::Count:
        break;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE: {
        CreateFonts(*state);
        state->animationsAvailable =
            ReadAnimationSettings(state->clientAnimations, state->menuAnimations);
        state->cleanupTargets = ResolveCleanupTargets();
        if (!ReadPointerSpeed(state->speed)) {
            state->status = L"Could not read pointer speed; adjust it and apply to set a new value.";
        }
        SetTimer(window, kAnimationTimer, 33, nullptr);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 980;
        info->ptMinTrackSize.y = 760;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        HDC memoryDc = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        DrawWindow(memoryDc, *state, width, height);
        BitBlt(dc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            state->animationPhase += 0.045f;
            if (state->animationPhase > 6.283185f) {
                state->animationPhase -= 6.283185f;
            }
            const float target = state->hover == HoverItem::None ? 0.0f : 1.0f;
            state->hoverMix += (target - state->hoverMix) * 0.24f;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (state->draggingSlider) {
            SetSpeedFromPoint(*state, point.x);
        }
        UpdateHover(*state, point);
        TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hover = HoverItem::None;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        state->focus = FocusFromPoint(*state, point);
        state->pressedItem = state->focus;
        state->mouseDown = true;
        if (Contains(state->layout.slider, point)) {
            state->draggingSlider = true;
            SetCapture(window);
            SetSpeedFromPoint(*state, point.x);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const FocusItem pressed = state->pressedItem;
        const bool activated = state->mouseDown && pressed == FocusFromPoint(*state, point);
        state->mouseDown = false;
        if (state->draggingSlider) {
            state->draggingSlider = false;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            SetSpeedFromPoint(*state, point.x);
        } else if (activated && pressed == FocusItem::ApplySpeed &&
                   Contains(state->layout.applySpeed, point)) {
            ApplyPointerSpeed(*state, window);
        } else if (activated && pressed == FocusItem::Animations &&
                   Contains(state->layout.animations, point)) {
            ApplyAnimationSettings(*state, window);
        } else if (activated && pressed == FocusItem::Clean &&
                   Contains(state->layout.clean, point)) {
            StartCleanup(*state, window);
        } else {
            for (std::size_t i = 0; i < state->layout.cleanupOptions.size(); ++i) {
                const FocusItem option = static_cast<FocusItem>(
                    static_cast<int>(FocusItem::UserTemp) + static_cast<int>(i));
                if (activated && pressed == option &&
                    Contains(state->layout.cleanupOptions[i], point)) {
                    state->selectedCleanup[i] = !state->selectedCleanup[i];
                    break;
                }
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        state->draggingSlider = false;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_TAB) {
            const int count = static_cast<int>(FocusItem::Count);
            const int current = static_cast<int>(state->focus);
            state->focus = static_cast<FocusItem>((current + 1) % count);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT) {
            if (state->focus == FocusItem::Slider) {
                const int delta = wParam == VK_RIGHT ? 1 : -1;
                state->speed = std::max(kMinimumSpeed, std::min(kMaximumSpeed,
                                                                state->speed + delta));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            ActivateFocusedItem(*state, window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        return 0;
    case kCleanupComplete: {
        std::unique_ptr<CleanupResult> result(reinterpret_cast<CleanupResult*>(lParam));
        state->cleaning = false;
        if (result) {
            if (result->errors == 0 && result->skipped == 0) {
                state->status = L"Cleanup finished: " + std::to_wstring(result->removed) +
                                L" files/folders removed.";
            } else {
                state->status = L"Cleanup finished: " + std::to_wstring(result->removed) +
                    L" removed, " + std::to_wstring(result->skipped) + L" skipped, " +
                    std::to_wstring(result->errors) + L" errors.";
            }
            std::wstring report = L"Cleanup finished.\n\nRemoved: " +
                std::to_wstring(result->removed) + L"\nSkipped (locked, inaccessible, or linked): " +
                std::to_wstring(result->skipped) + L"\nOther errors: " +
                std::to_wstring(result->errors);
            if (result->firstError != ERROR_SUCCESS) {
                report += L"\nFirst Windows error code: " + std::to_wstring(result->firstError);
            }
            MessageBoxW(window, report.c_str(), L"Cleanup results",
                        result->errors == 0 ? MB_ICONINFORMATION | MB_OK : MB_ICONWARNING | MB_OK);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(window, kAnimationTimer);
        DestroyFonts(*state);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, L"Could not register the application window.",
                    L"Sigma Tweaks", MB_ICONERROR | MB_OK);
        return 1;
    }

    AppState state;
    HWND window = CreateWindowExW(0, kWindowClass, L"Sigma Tweaks",
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1180, 850,
                                  nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Could not create the application window.",
                    L"Sigma Tweaks", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
