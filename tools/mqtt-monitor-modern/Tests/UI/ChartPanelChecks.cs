using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void ChartPanelChecks()
    {
        var now = Telemetry.Now;
        var key = new MetricKey("isolated", "MEMORY", "", "RSS_KB");
        var panel = new ChartPanel("Memory");
        var window = new Window { Width = 700, Height = 450, Content = panel };
        var metric = new MetricView(key, "2048", now, [new Sample(now, now, 2048)]);
        panel.Update([metric], null, 300, "isolated");
        window.Show();
        Pump();
        var chart = panel.GetVisualDescendants().OfType<TrendChart>().Single();
        Check(chart.Unit == "MiB" && chart.Lines.Single().Points.Single().Value == 2,
            "chart panels can display supplied data independently of the main window");
        Click(window, "Settings");
        Pump();
        var dialog = window.OwnedWindows.OfType<ChartSettingsDialog>().Single();
        Check(dialog.GetVisualDescendants().OfType<CheckBox>().Count() == 1,
            "standalone chart settings use the supplied topic and report");
        dialog.Close();
        panel.Select("Selected metric");
        panel.Update([metric], key, 300, "isolated");
        Pump();
        Check(chart.Unit == "KiB" && chart.Lines.Single().Points.Single().Value == 2048,
            "selected metrics keep raw units instead of report conversions");
        window.Close();

        var reflectors = new MetricStore();
        for (var i = 0; i < 8; i++)
            reflectors.Accept("hint", $"R_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_AVG_OWD_DELTA_US\nR; {now:R}; 10.0.0.{i}; 2000", now);
        var latency = new ChartPanel("Latency");
        var latencyWindow = new Window { Width = 900, Height = 500, Content = latency };
        latencyWindow.Show();
        latency.Update(reflectors.Snapshot(300, now), null, 300, "hint");
        Pump();
        var hint = latency.GetVisualDescendants().OfType<TextBlock>().Single(t => t.Text == UiText.AutomaticHint);
        var latencyChart = latency.GetVisualDescendants().OfType<TrendChart>().Single();
        Check(hint.IsVisible && hint.Bounds.Top >= latencyChart.Bounds.Bottom,
            "the automatic-trace hint sits under the chart, not in the legend");
        latencyWindow.Close();
    }
}
