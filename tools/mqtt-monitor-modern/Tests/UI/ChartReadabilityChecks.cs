using Avalonia;
using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void ChartReadabilityChecks()
    {
        // Four readings per second for 3.5 s (exact binary times): each whole clock second
        // is one column with its extremes and mean; the last, partial second holds two.
        (double, Point)[] Readings(double first, double shift) => Enumerable.Range(0, 14)
            .Select(i => (first + i * .25, new Point(shift + i, i % 3 == 0 ? 10 : 50))).ToArray();
        var columns = PlotEnvelope.Reduce(Readings(100, 0), 1);
        Check(columns.Length == 4 && columns.Take(3).All(c => c.Top == 10 && c.Bottom == 50 && c.Count == 4) &&
              columns[3].Count == 2 && columns.Sum(c => c.Count) == 14,
            "envelope columns keep every reading's highest and lowest position per clock second");
        Check(columns[0].Mean == 30 && columns[1].Mean == 40, "an envelope column's mean averages its second's readings");
        // Scrolling moves every reading left by the same amount; columns must not regroup.
        var scrolled = PlotEnvelope.Reduce(Readings(100, -0.7), 1);
        Check(columns.Zip(scrolled).All(pair => pair.First.Count == pair.Second.Count && pair.First.Mean == pair.Second.Mean &&
                                               Math.Abs(pair.First.X - pair.Second.X - 0.7) < 1e-9),
            "a scrolling window keeps each reading in the same one-second column");
        var late = PlotEnvelope.Reduce(Readings(100.5, 0), 1);
        Check(late.Select(c => c.Count).SequenceEqual([2, 4, 4, 4]), "columns follow clock seconds, not the first visible reading");
        Check(PlotEnvelope.Reduce([], 1).Length == 0, "an empty trace has no columns");
        Check(PlotEnvelope.Step(300, 440) == 1 && PlotEnvelope.Step(60, 440) == 1 && PlotEnvelope.Step(900, 440) == 5 &&
              PlotEnvelope.Step(3600, 440) == 10,
            "charts average over one second unless a second is narrower than a pixel");

        var now = Telemetry.Now;
        TrendChart Clip(string report, double spike)
        {
            var store = new MetricStore();
            for (var i = 0; i < 200; i++)
            {
                var value = i == 150 ? spike : 2000 + i % 7 * 100;
                var at = now - 200 + i;
                store.Accept("clip", report == ReportNames.TcpQueues
                    ? $"TCP_QUEUE; date; {at:R}; {at:R}; 1; {value:R}; 1; 2000"
                    : $"R_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_AVG_OWD_DELTA_US\nR; {at:R}; 1.1.1.1; {value:R}", at);
            }
            var panel = new ChartPanel(report);
            var owner = new Window { Width = 900, Height = 500, Content = panel };
            owner.Show();
            panel.Update(store.Snapshot(300, now), null, 300, "clip");
            Pump();
            var chart = panel.GetVisualDescendants().OfType<TrendChart>().Single();
            owner.Close();
            return chart;
        }
        TrendChart Latency(double spike) => Clip(ReportNames.Latency, spike);
        var spiky = Latency(260000);
        Check(spiky.Ceiling is > 2 and < 4, "a lone latency spike tops the scale just above the 99th percentile (ms)");
        Check(Latency(2700).Ceiling == null, "latency without outlying spikes keeps its full range");
        Check(Clip(ReportNames.TcpQueues, 377000).Ceiling is > 2 and < 4 && Clip(ReportNames.TcpQueues, 2700).Ceiling == null,
            "TCP queues clip a lone spike like latency, and keep the full range without one");
        var red = Avalonia.Media.Colors.Red;
        var blue = Avalonia.Media.Colors.Blue;
        // Labels 30 px wide at left edges 0/20/30/100 with 5 px spacing: the first three crowd.
        var labels = ClipLabels.Merge([new(30, 30, 51, red), new(0, 30, 260, blue), new(20, 30, 90, red), new(100, 30, 75, blue)], 5);
        Check(labels.Length == 2 && labels[0] == new ClipLabel(0, 30, 260, blue) && labels[1] == new ClipLabel(100, 30, 75, blue),
            "crowding peak labels merge into the highest value");
        var spaced = ClipLabels.Merge([new(0, 30, 10, red), new(35, 30, 20, red), new(64, 30, 30, red)], 5);
        Check(spaced.Length == 2 && spaced[0].Value == 10 && spaced[1].Value == 30,
            "labels 5 px apart edge to edge both stay; a closer one merges into the higher");
        var later = ClipLabels.Merge([new(0, 30, 10, red), new(36, 30, 99, red), new(70, 30, 20, red)], 5);
        Check(later.Length == 2 && later[0].Value == 10 && later[1] == new ClipLabel(36, 30, 99, red),
            "a higher label merged from the right keeps its own place and spacing");
        Check(ClipLabels.Merge([], 5).Length == 0, "no clipped spikes, no labels");
        var throughput = new ChartPanel("Throughput");
        throughput.Update([], null, 300, "clip");
        Check(throughput.GetVisualDescendants().OfType<TrendChart>().All(c => c.Ceiling == null) &&
            new TrendChart().Ceiling == null, "only latency and TCP queue charts clip their scale");

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
