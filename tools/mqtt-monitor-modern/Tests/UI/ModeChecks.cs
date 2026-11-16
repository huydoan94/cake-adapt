using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void WaitFor(Task task)
    {
        for (var i = 0; i < 2000 && !task.IsCompleted; i++)
        {
            Pump();
            Thread.Sleep(5);
        }
    }

    private static void ModeChecks()
    {
        var window = new MainWindow { Width = 1440, Height = 920 };
        window.Show();
        Pump();
        T Find<T>(Func<T, bool> match) where T : Control => window.GetVisualDescendants().OfType<T>().First(match);
        var strip = window.GetVisualDescendants().OfType<TimelineStrip>().Single();
        Check(!strip.IsVisible && !window.InHistory, "live mode has no timeline strip");

        var store = HistoryFixture();
        window.EnterHistory(store, ["mqtt-2026-11-14.jsonl"]);
        Pump();
        var timeRange = Find<ComboBox>(c => c.ItemsSource == ChartDefaults.WindowLabels);
        var pause = Find<ToggleButton>(b => Equals(b.Content, UiText.Pause));
        var connect = Find<Button>(b => Equals(b.Content, UiText.ConnectBroker));
        var startOver = Find<Button>(b => Equals(b.Content, UiText.StartOver));
        Check(window.InHistory && strip.IsVisible && startOver.IsVisible && !timeRange.IsVisible && !pause.IsVisible && !connect.IsVisible,
            "history mode shows the strip and Start over, and hides the live controls");
        Check(strip.DataStart == store.Start && strip.DataEnd == store.End && strip.RangeEnd == store.End &&
              strip.RangeStart == Math.Max(store.Start, store.End - HistoryModeDefaults.InitialRangeSeconds),
            "history opens on the last hour of the loaded data");
        Check(window.GetVisualDescendants().OfType<TextBlock>().Any(t => t.Text?.Contains("mqtt-2026-11-14.jsonl") == true),
            "the header names the loaded files");

        var startingOver = window.StartOverAsync();
        Pump();
        Check(!window.InHistory && !strip.IsVisible && timeRange.IsVisible && connect.IsVisible && window.OwnedWindows.OfType<StartWindow>().Any(),
            "Start over drops the history and asks for the mode again");
        window.OwnedWindows.OfType<StartWindow>().Single().Close();
        WaitFor(startingOver);

        var opened = Directory.CreateTempSubdirectory("cake-open-");
        try
        {
            var day = Path.Combine(opened.FullName, "router-b.jsonl");
            File.WriteAllText(day, CaptureDay(new DateTimeOffset(2026, 11, 14, 0, 0, 0, TimeSpan.Zero), 600));
            var direct = new MainWindow { Width = 1440, Height = 920, AskModeOnOpen = true, StartFile = day };
            direct.Show();
            for (var i = 0; i < 2000 && !direct.InHistory; i++)
            {
                Pump();
                Thread.Sleep(5);
            }
            Check(direct.InHistory && !direct.OwnedWindows.OfType<StartWindow>().Any(),
                "a capture file given at start opens straight in history mode");
            direct.Close();
        }
        finally { opened.Delete(true); }

        var folder = Directory.CreateTempSubdirectory("cake-cancel-");
        try
        {
            var big = Path.Combine(folder.FullName, "mqtt-2026-11-14.jsonl");
            File.WriteAllText(big, CaptureDay(new DateTimeOffset(2026, 11, 14, 0, 0, 0, TimeSpan.Zero), 200000));
            var owner = new Window();
            owner.Show();
            var load = new LoadWindow();
            var loading = load.RunAsync(owner, [big]);
            Pump();
            load.Cancel();
            WaitFor(loading);
            Check(loading.IsCompleted && loading.Result == null && !owner.OwnedWindows.OfType<LoadWindow>().Any(),
                "cancelling a load returns no history and closes the loading window");
            owner.Close();
        }
        finally { folder.Delete(true); }
    }
}
