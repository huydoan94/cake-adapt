using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using Avalonia.Threading;
using CakeAdaptMonitor;

static partial class Tests
{
    private static string? HeaderText(DataGridColumn column) => (column.Header as TextBlock)?.Text;

    private static void MetricTableChecks(MainWindow window, MetricStore store, string path)
    {
        var navigate = window.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "All metrics"));
        navigate.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Dispatcher.UIThread.RunJobs();
        var table = window.GetVisualDescendants().OfType<DataGrid>().Single();
        Check(table.ItemsSource.Cast<MetricRow>().Any(), "metric navigation shows received metrics");
        Check(table.Columns.Take(3).Select(HeaderText).SequenceEqual(["Metric", "Topic", "Reflector / CPU"]),
            "identity columns come first so duplicate metric names are told apart");
        Check(MetricColumns.All.Where(c => c.Width.IsAbsolute).Sum(c => c.Width.Value) + MetricColumns.RemainderMinimumWidth <= 1280 - 172 - 48 - 12,
            "all table columns and the scroll bar fit a maximized 1280-pixel window");
        Check(Telemetry.Format(125000) == "125000" && Telemetry.Format(126071.214) == "126071.21",
            "statistics use the same plain number format as Latest values");
        store.Accept("cake-adapt/z-second", $"MEMORY; date; {Telemetry.Now:R}; 100; 1000; 1200; 700; 1500", Telemetry.Now);
        window.Refresh();
        Pump();
        var rss = table.ItemsSource.Cast<MetricRow>().Where(r => r.Name == "RSS_KB").Select(r => r.Topic).ToArray();
        Check(rss.SequenceEqual(rss.Order(StringComparer.Ordinal)) && rss.Length >= 2,
            "rows of one metric are grouped and ordered by source");
        var batchTime = Telemetry.Now;
        store.PublishBatch();
        store.Accept("cake-adapt/sample/main", "BATCH_UI_HEADER; LOG_TIMESTAMP; BATCH_UI_VALUE", batchTime);
        store.Accept("cake-adapt/sample/main", $"BATCH_UI; {batchTime:R}; 91.123456789", batchTime);
        store.Accept("cake-adapt/sample/main", $"BATCH_UI; {batchTime:R}; 1", batchTime);
        store.PublishBatch();
        window.Refresh();
        Pump();
        var batchRow = table.ItemsSource.Cast<MetricRow>().Single(r => r.Name == "BATCH_UI_VALUE");
        Check(batchRow.Latest == "91.123456789" && table.Columns.Any(c => HeaderText(c) == "Latest"),
            "All metrics keeps its Latest label and shows the received batch peak");

        // At the minimum window width the table scrolls in both directions.
        window.Width = UiLayout.MinimumWindowWidth;
        Pump();
        var columnNames = Enumerable.Range(0, 140).Select(i => $"SCROLL_{i:000}").ToArray();
        store.Accept("cake-adapt/sample/main", "SCROLL_HEADER; LOG_TIMESTAMP; " + string.Join("; ", columnNames) +
            $"\nSCROLL; {Telemetry.Now:R}; " + string.Join("; ", Enumerable.Repeat("42", 140)), Telemetry.Now);
        window.Refresh();
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        var selectedRow = table.ItemsSource.Cast<MetricRow>().Single(r => r.Name == "SCROLL_100");
        table.SelectedItem = selectedRow;
        table.ScrollIntoView(selectedRow, table.Columns.Last());
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        var vertical = table.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.ScrollBar>()
            .Single(b => b.Orientation == Avalonia.Layout.Orientation.Vertical);
        var horizontal = table.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.ScrollBar>()
            .Single(b => b.Orientation == Avalonia.Layout.Orientation.Horizontal);
        var beforeVertical = vertical.Value;
        var beforeHorizontal = horizontal.Value;
        Check(beforeVertical > 0 && beforeHorizontal > 0, "scroll regression exercises both axes");
        var originalSource = table.ItemsSource;
        store.Accept("cake-adapt/sample/main", $"SCROLL; {Telemetry.Now:R}; " + string.Join("; ", Enumerable.Repeat("43", 140)), Telemetry.Now);
        store.PublishBatch();
        for (var i = 0; i < 3; i++)
        {
            window.Refresh();
            Dispatcher.UIThread.RunJobs();
            AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        }
        Check(ReferenceEquals(originalSource, table.ItemsSource) && ReferenceEquals(selectedRow, table.SelectedItem), "refresh preserves collection and selected row");
        Check(Math.Abs(vertical.Value - beforeVertical) < .01 && Math.Abs(horizontal.Value - beforeHorizontal) < .01, "refresh preserves vertical and horizontal scroll");
        Check(selectedRow.Latest == "43", "stable rows still receive new readings");
        var search = window.GetVisualDescendants().OfType<TextBox>().Single(t => t.Watermark?.ToString()?.StartsWith("Find a metric") == true);
        search.Text = "RSS_KB";
        Dispatcher.UIThread.RunJobs();
        Check(table.ItemsSource.Cast<MetricRow>().All(m => m.Name.Contains("RSS_KB")), "metric search filters table");
        window.Width = 1440;
        Pump();
        using var metricBitmap = window.CaptureRenderedFrame();
        metricBitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "metrics-preview.png"));
    }

}
