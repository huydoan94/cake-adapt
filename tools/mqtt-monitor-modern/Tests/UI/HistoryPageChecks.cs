using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void HistoryPageChecks()
    {
        const double start = 1_800_000_000;
        var window = new MainWindow { Width = 1440, Height = 920 };
        window.Show();
        window.EnterHistory(HistoryFixture(start), ["fixture.jsonl"]);
        window.SetHistoryRange(start + 30, start + 89);
        Pump();
        var chart = window.GetVisualDescendants().OfType<TrendChart>().First();
        Check(chart.EndTime == start + 89 && chart.Seconds == 59 && chart.RangeTicks, "history charts show exactly the chosen range");
        var download = CardLines(window, "DOWNLOAD");
        var peakClock = DateTimeOffset.FromUnixTimeSeconds((long)start + 89).ToLocalTime().ToString("HH:mm:ss");
        Check(download.Length == 2 && download[0].Value == "1.09 Mbit/s" && download[0].Caption.StartsWith("a") &&
              download[0].Caption.Contains("peak " + peakClock),
            "history cards show the range peak and when it happened");
        window.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "All metrics")).RaiseEvent(new Avalonia.Interactivity.RoutedEventArgs(Button.ClickEvent));
        Pump();
        var table = window.GetVisualDescendants().OfType<DataGrid>().Single();
        var row = table.ItemsSource.Cast<MetricRow>().First(r => r.Topic == "a" && r.Name == MetricNames.DownloadTraffic);
        Check(!table.Columns.Single(c => HeaderText(c) == "Age").IsVisible && row.Max == "1089" && row.Min == "1030" && row.Count == 60,
            "the history table shows exact range statistics without Age");
        var events = window.GetVisualDescendants().OfType<TextBox>().Single(t => t.IsReadOnly);
        Check(events.Text?.Contains("line load changed") == true, "Events lists events inside the range");
        window.SetHistoryRange(start + 70, start + 119);
        Pump();
        Check(events.Text?.Contains("line load changed") != true, "Events leaves out events outside the range");
        window.Close();

        MetricView Reflector(string name, long readings) => new(new MetricKey("a", "DATA", name, MetricNames.DownloadDelay), "1", 100,
            [new Sample(1, 1, 1)]) { Stats = new RangeStats(1, 1, 1, 1, readings, 1, readings == 5 ? 900 : 100) };
        var candidates = Enumerable.Range(0, 8).Select(i => Reflector("r" + i, i == 7 ? 5 : 100 + i)).ToArray();
        var chosen = ReportCatalog.Defaults(candidates, view => view.Stats?.Count ?? 0);
        Check(chosen.Length == 6 && chosen.All(v => v.Dimension != "r7" && v.Dimension != "r0"),
            "history picks automatic traces with the most readings in the range");
    }
}
