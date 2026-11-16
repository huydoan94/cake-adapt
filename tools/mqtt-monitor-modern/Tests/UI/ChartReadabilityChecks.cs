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

        // Bufferbloat: one tick row and one count per source on the Latency chart only.
        var bloat = new[]
        {
            new BloatView("cake-adapt/router-a", "DL", 2, [new BloatBucket(now - 20, 2)]),
            new BloatView("cake-adapt/router-a", "UL", 0, []),
            new BloatView("cake-adapt/router-b", "DL", 5, [new BloatBucket(now - 10, 3), new BloatBucket(now - 4, 2)]),
            new BloatView("cake-adapt/router-b", "UL", 1, [new BloatBucket(now - 4, 1)])
        };
        var latencyStore = new MetricStore();
        latencyStore.Accept("cake-adapt/router-a", $"R_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_AVG_OWD_DELTA_US\nR; {now:R}; 1.1.1.1; 2000", now);
        var latencyPanel = new ChartPanel("Latency");
        var latencyOwner = new Window { Width = 900, Height = 500, Content = latencyPanel };
        latencyOwner.Show();
        latencyPanel.Update(latencyStore.Snapshot(300, now), null, 300, ChartDefaults.AllSources, bloat);
        Pump();
        var marks = latencyPanel.GetVisualDescendants().OfType<TrendChart>().Single().Marks;
        Check(marks.Length == 2 && marks[0].Times.Length == 1 && marks[1].Times.SequenceEqual([now - 10, now - 4]),
            "latency chart draws one bufferbloat tick row per source");
        var texts = latencyPanel.GetVisualDescendants().OfType<TextBlock>().Select(t => t.Text).ToArray();
        Check(texts.Contains("cake-adapt/router-a  DL 2 · UL 0") && texts.Contains("cake-adapt/router-b  DL 5 · UL 1"),
            "latency legend shows each source's download and upload bufferbloat counts");
        latencyOwner.Close();
        var throughputPanel = new ChartPanel("Throughput");
        var throughputOwner = new Window { Width = 900, Height = 500, Content = throughputPanel };
        throughputOwner.Show();
        throughputPanel.Update([], null, 300, ChartDefaults.AllSources, bloat);
        Pump();
        Check(throughputPanel.GetVisualDescendants().OfType<TrendChart>().Single().Marks.Length == 0,
            "bufferbloat marks appear only on the latency chart");
        throughputOwner.Close();
    }
}
