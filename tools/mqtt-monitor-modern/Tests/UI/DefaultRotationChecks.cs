using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void DefaultRotationChecks()
    {
        var now = Telemetry.Now;
        var store = new MetricStore();
        void Receive(string reflector, double time) => store.Accept("rotation",
            $"R_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_AVG_OWD_DELTA_US\nR; {time:R}; {reflector}; 1000", time);
        for (var i = 0; i < 6; i++)
            Receive("old-" + i, now - 60);
        var panel = new ChartPanel("Latency");
        var owner = new Window { Width = 900, Height = 500, Content = panel };
        void Refresh() => panel.Update(store.Snapshot(300, now), null, 300, "rotation");
        panel.RefreshRequested += Refresh;
        Refresh(); owner.Show(); Pump();
        var chart = panel.GetVisualDescendants().OfType<TrendChart>().Single();
        ChartSettingsDialog Open()
        {
            Click(owner, "Settings"); Pump();
            return owner.OwnedWindows.OfType<ChartSettingsDialog>().Single();
        }
        for (var i = 0; i < 6; i++)
            Receive("new-" + i, now - 10 + i);
        Refresh(); Pump();
        Check(chart.Lines.Length == 6 && chart.Lines.All(line => line.Label.Contains("new-")),
            "automatic charts follow reflector rotation without pressing Use defaults");
        var dialog = Open();
        var options = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(options.Count(option => option.IsChecked == true) == 6 &&
            options.Where(option => option.IsChecked == true).All(option => ((TraceKey)option.Tag!).Dimension.StartsWith("new-")),
            "settings checks the currently active automatic defaults");
        foreach (var option in options)
            option.IsChecked = ((TraceKey)option.Tag!).Dimension == "old-0";
        Click(dialog, "Apply"); Pump();
        Receive("replacement", now);
        Refresh(); Pump();
        Check(chart.Lines.Single().Label.Contains("old-0"),
            "manual reflector choices stay pinned during rotation");
        dialog = Open(); Click(dialog, "Use defaults"); Pump();
        Check(chart.Lines.Length == 6 && chart.Lines.Any(line => line.Label.Contains("replacement")) &&
            chart.Lines.All(line => !line.Label.Contains("old-") && !line.Label.Contains("new-0")),
            "Use defaults restores the latest six active traces instead of stale choices");
        Receive("old-0", now);
        Refresh(); Pump();
        Check(chart.Lines.Any(line => line.Label.Contains("old-0")),
            "automatic defaults continue following activity after Use defaults");
        Check(store.Snapshot(300, now).Length == 13,
            "rotation selection preserves historical series for manual settings");
        for (var i = 0; i < 20; i++)
            Receive("extra-" + i, now - 30);
        Refresh(); Pump();
        dialog = Open(); Click(dialog, "First 24"); Click(dialog, "Apply"); Pump();
        var pinned = chart.Lines.Select(line => line.Label).ToArray();
        Check(pinned.Length == 24, "First 24 applies an explicit selection of 24 traces");
        Receive("newest-reflector", now);
        Refresh(); Pump();
        Check(chart.Lines.Select(line => line.Label).SequenceEqual(pinned),
            "First 24 stays pinned when new reflectors arrive");
        dialog = Open();
        Check(dialog.GetVisualDescendants().OfType<CheckBox>().Count(option => option.IsChecked == true) == 24,
            "settings retains all 24 explicit choices after rotation");
        Click(dialog, "Clear"); Click(dialog, "Apply"); Pump();
        Receive("another-reflector", now);
        Refresh(); Pump();
        Check(chart.Lines.Length == 0,
            "an explicit empty selection never reverts to automatic defaults");
        owner.Close();
    }
}
