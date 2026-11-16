using Avalonia.Controls;
using Avalonia.Controls.Presenters;
using Avalonia.Headless;
using Avalonia.Interactivity;
using Avalonia.Threading;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void DialogAndEventChecks(MainWindow window, string path)
    {
        var eventsNav = window.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Events"));
        eventsNav.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        var events = window.GetVisualDescendants().OfType<TextBox>().Single(t => t.IsReadOnly);
        Check(events.Text?.Contains("cake-adapt/sample/main") ?? false, "event view contains raw telemetry");
        Check(events.GetVisualDescendants().OfType<TextPresenter>().Any(p => p.IsVisible && p.Bounds.Height > 0),
            "event view has a visible text presenter, so its text is drawn");
        var lines = events.Text!.Split(UiText.EventSeparator);
        Check(lines.Length < 2 || string.CompareOrdinal(lines[0][..8], lines[^1][..8]) >= 0, "newest events are listed first");
        var dialog = new ConnectionDialog(new BrokerSettings());
        dialog.Show(window);
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var dialogBitmap = dialog.CaptureRenderedFrame();
        dialogBitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "connection-preview.png"));
        Check(dialog.GetVisualDescendants().OfType<CheckBox>().Single().IsChecked == true, "TLS enabled in connection dialog");
        dialog.Close();
    }

}
