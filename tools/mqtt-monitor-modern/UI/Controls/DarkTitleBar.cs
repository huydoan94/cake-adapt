using System.Runtime.InteropServices;
using Avalonia.Controls;

namespace CakeAdaptMonitor;

// Windows draws the title bar itself and keeps it light unless the window asks
// DWM for immersive dark mode. Other platforms are left unchanged.
internal static class DarkTitleBar
{
    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 since Windows 10 20H1, 19 on 1809-1909.
    private const int DarkModeAttribute = 20, LegacyDarkModeAttribute = 19;

    public static void Apply(Window window) => window.Opened += (_, _) =>
    {
        if (!OperatingSystem.IsWindows() || window.TryGetPlatformHandle()?.Handle is not { } handle || handle == IntPtr.Zero)
            return;
        var enabled = 1;
        if (DwmSetWindowAttribute(handle, DarkModeAttribute, ref enabled, sizeof(int)) != 0)
            DwmSetWindowAttribute(handle, LegacyDarkModeAttribute, ref enabled, sizeof(int));
    };

    // DllImport keeps the project free of unsafe code, which LibraryImport would require.
    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);
}
