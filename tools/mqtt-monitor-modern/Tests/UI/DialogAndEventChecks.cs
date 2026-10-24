using Avalonia.Controls;
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
        Check(window.GetVisualDescendants().OfType<TextBox>().Any(t => t.IsReadOnly && (t.Text?.Contains("cake-adapt/sample/main") ?? false)), "event view contains raw telemetry");
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
