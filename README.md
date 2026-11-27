# Sigma Tweaks

Sigma Tweaks is a small native Windows 10/11 desktop utility for a few clearly scoped, reversible settings. It uses the Win32 API and GDI directly; it has no third-party runtime or administrator requirement.

## Features

- **Pointer speed:** adjust the built-in Windows mouse speed on a 1–20 scale and apply it after confirmation.
- **UI animations:** turn Windows client-area and menu animations off or restore them. This is a comfort/visual-effects setting, not a promise of measurable performance gains.
- **Temporary-file cleanup:** choose the current account's `%TEMP%` folder, `Windows\Temp`, and/or `Windows\Prefetch` individually. The selected folders remain in place. The app confirms the exact resolved paths, never requests elevation, does not follow directory links, and reports removed, skipped, and failed items. Locked or inaccessible entries are left alone.

The interface includes animated hover states, keyboard-operable controls, and a high-contrast dark palette. No settings are changed and no files are removed until you confirm the corresponding action.

## Build

Use Visual Studio 2022 with the **Desktop development with C++** workload and CMake 3.20 or newer:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is written to `build\Release\SigmaTweaks.exe`. The application manifest requests `asInvoker` execution and enables long-path support.
