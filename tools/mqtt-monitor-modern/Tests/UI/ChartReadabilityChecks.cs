using Avalonia;
using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void ChartReadabilityChecks()
    {
        // Ten points over 9 px: three 3-px columns keep each column's extremes and mean.
        var points = Enumerable.Range(0, 10).Select(i => new Point(i, i % 2 == 0 ? 10 : 50)).ToArray();
        var columns = PlotEnvelope.Reduce(points, 3);
        Check(columns.Length == 4 && columns.Take(3).All(c => c.Top == 10 && c.Bottom == 50) && columns[3].Count == 1 && columns.Sum(c => c.Count) == 10,
            "envelope columns keep every point's highest and lowest position");
        Check(PlotEnvelope.Dense(10, 4) && !PlotEnvelope.Dense(5, 4) && PlotEnvelope.Reduce([], 3).Length == 0,
            "envelopes are used only for dense traces");

        var now = Telemetry.Now;
        TrendChart Latency(double spike)
        {
            var store = new MetricStore();
            for (var i = 0; i < 200; i++)
            {
                var value = i == 150 ? spike : 2000 + i % 7 * 100;
                store.Accept("clip", $"R_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_AVG_OWD_DELTA_US\nR; {now - 200 + i:R}; 1.1.1.1; {value:R}", now - 200 + i);
            }
            var panel = new ChartPanel("Latency");
            var owner = new Window { Width = 900, Height = 500, Content = panel };
            owner.Show();
            panel.Update(store.Snapshot(300, now), null, 300, "clip");
            Pump();
            var chart = panel.GetVisualDescendants().OfType<TrendChart>().Single();
            owner.Close();
            return chart;
        }
        var spiky = Latency(260000);
        Check(spiky.Ceiling is > 2 and < 4, "a lone latency spike tops the scale just above the 99th percentile (ms)");
        Check(Latency(2700).Ceiling == null, "latency without outlying spikes keeps its full range");
        var throughput = new ChartPanel("Throughput");
        throughput.Update([], null, 300, "clip");
        Check(throughput.GetVisualDescendants().OfType<TrendChart>().All(c => c.Ceiling == null) &&
            new TrendChart().Ceiling == null, "only latency charts clip their scale");
    }
}
