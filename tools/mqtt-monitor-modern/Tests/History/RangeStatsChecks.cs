using CakeAdaptMonitor;

static partial class Tests
{
    // Range statistics read from the summary levels must equal statistics computed
    // from the raw readings, for any range, at both detail tiers.
    private static void RangeStatsChecks()
    {
        var random = new Random(7);
        foreach (var step in new[] { HistoryModeDefaults.DetailedStepSeconds, HistoryModeDefaults.CoarseStepSeconds })
        {
            const double origin = 1_800_000_000;
            var readings = new List<(double Time, double Value)>();
            var series = new BucketSeries(step);
            for (var t = origin; t < origin + 2 * 86400; t += 0.2 + random.NextDouble())
            {
                var value = random.Next(0, 1000);
                readings.Add((t, value));
                series.Add(t, value);
            }
            series.Seal();
            var failures = 0;
            for (var i = 0; i < 200; i++)
            {
                var a = origin - 100 + random.NextDouble() * (2 * 86400 + 200);
                var b = a + Math.Pow(10, random.NextDouble() * 5.3);
                var inside = readings.Where(r => Math.Floor(r.Time / step) * step + step > a && Math.Floor(r.Time / step) * step <= b).ToList();
                var stats = series.Stats(a, b);
                if (inside.Count == 0)
                {
                    failures += stats == null ? 0 : 1;
                    continue;
                }
                var max = inside.Max(r => r.Value);
                var expectedMaxTime = Math.Floor(inside.First(r => r.Value == max).Time / step) * step;
                var ok = stats != null && stats.Count == inside.Count && stats.Min == inside.Min(r => r.Value) && stats.Max == max &&
                    stats.MaxTime == expectedMaxTime && stats.Last == inside[^1].Value &&
                    stats.LastTime == Math.Floor(inside[^1].Time / step) * step &&
                    Math.Abs(stats.Mean - inside.Average(r => r.Value)) < 1e-3;
                failures += ok ? 0 : 1;
            }
            Check(failures == 0, $"range statistics from summary levels match the raw readings ({step} s detail)");
        }
    }
}
