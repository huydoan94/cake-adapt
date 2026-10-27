using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Styling;
using Avalonia.Threading;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void Preview(string path)
    {
        AppBuilder.Configure<App>().UseSkia().WithInterFont()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false }).SetupWithoutStarting();
        var store = SampleTelemetry();
        var window = new MainWindow { Width = 1440, Height = 920 };
        window.Preview(store);
        window.Show();
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var bitmap = window.CaptureRenderedFrame();
        if (bitmap == null)
            throw new Exception("Preview capture failed");
        bitmap.Save(path);
        Check(bitmap.PixelSize.Width > 1000, "dark dashboard rendered");
        Check(Application.Current!.RequestedThemeVariant == ThemeVariant.Dark, "dark theme enforced");
        Check(!window.GetVisualDescendants().OfType<Button>().Any(b => Equals(b.Content, "Export CSV")), "viewer has no CSV export control");
        DashboardChecks(window, store);
        ChartSettingsFlow(window, store, path);
        MetricTableChecks(window, store, path);
        DialogAndEventChecks(window, path);
        ChartRenderingChecks(window, path);
        ChartPanelChecks();
        DefaultRotationChecks();
        window.Close();
    }

}
