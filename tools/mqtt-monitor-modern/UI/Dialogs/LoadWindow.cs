using Avalonia.Controls;
using Avalonia.Layout;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

// Progress and Cancel while capture files load; returns the store, or null when
// cancelled or failed (Error then holds the reason).
public sealed class LoadWindow : Window
{
    private readonly CancellationTokenSource cancel = new();
    private readonly TextBlock progressText = Label(UiText.LoadingStart, UiTypography.Body);
    private readonly ProgressBar bar = new() { Minimum = 0, Maximum = 1 };
    public string Error { get; private set; } = "";

    public LoadWindow()
    {
        Title = UiText.LoadingTitle;
        Width = UiLayout.LoadWidth;
        Height = UiLayout.LoadHeight;
        CanResize = false;
        Background = UiTheme.Panel;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        DarkTitleBar.Apply(this);
        var stop = ActionButton(UiText.Cancel);
        stop.HorizontalAlignment = HorizontalAlignment.Right;
        stop.Click += (_, _) => Cancel();
        var form = new StackPanel { Spacing = UiLayout.FormSpacing, Margin = UiLayout.ConnectionPadding };
        form.Children.Add(progressText);
        form.Children.Add(bar);
        form.Children.Add(stop);
        Content = form;
        Closing += (_, _) => cancel.Cancel();
    }

    public void Cancel() => cancel.Cancel();

    public async Task<HistoryStore?> RunAsync(Window owner, IReadOnlyList<string> paths)
    {
        _ = ShowDialog(owner);
        var progress = new Progress<LoadProgress>(report =>
        {
            bar.Value = report.Fraction;
            progressText.Text = string.Format(UiText.LoadingFormat, report.File, report.Files, report.Records, report.BadLines, report.Fraction);
        });
        try
        {
            var loaded = await CaptureLoader.LoadAsync(paths, progress, cancel.Token);
            // Cancel pressed after the load finished but before this resumed: still no session.
            return cancel.IsCancellationRequested ? null : loaded;
        }
        catch (OperationCanceledException)
        {
            return null;
        }
        catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException or InvalidDataException)
        {
            Error = e.Message;
            return null;
        }
        finally
        {
            Close();
        }
    }
}
