using CakeAdaptMonitor;

static partial class Tests
{
    private static void BucketSeriesChecks()
    {
        var hour = new BucketSeries(1);
        for (var t = 0; t < 3600; t++)
            hour.Add(t, t % 100);
        hour.Add(1800.5, 9999);
        hour.Seal();
        var all = hour.Stats(0, 3599)!;
        Check(all.Min == 0 && all.Max == 9999 && all.MaxTime == 1800 && all.Count == 3601 && all.Last == 99 && all.LastTime == 3599,
            "bucket series stats are exact over the range");
        var wide = hour.Read(0, 3600, 600);
        Check(wide.Length <= 1200 && wide.Max(s => s.Value) == 9999, "a wide read uses a coarser level and keeps spikes");
        Check(hour.Read(0, 3600, 6).Max(s => s.Value) == 9999, "even the coarsest level keeps a lone spike");
        var narrow = hour.Read(100, 160, 600);
        Check(narrow.Length > 0 && narrow.All(s => s.SourceTime >= 100 && s.SourceTime <= 160) && narrow.Min(s => s.Value) == 0,
            "a narrow read uses one-second buckets inside the range");
        // Panning by part of a group must not regroup the buckets the two ranges share.
        static (double, double?)[] Inside(Sample[] read, double from, double to) =>
            read.Where(s => s.SourceTime >= from && s.SourceTime < to).Select(s => (s.SourceTime, s.Value)).ToArray();
        var before = hour.Read(1000, 1600, 100);
        var panned = hour.Read(1003, 1603, 100);
        Check(Inside(before, 1100, 1500).SequenceEqual(Inside(panned, 1100, 1500)),
            "panning a history range keeps its groups on fixed times");

        // End to end at the dashboard's sizes: a 5-hour range of noisy one-second
        // traffic, read with the history column count and grouped as the chart draws it.
        var traffic = new BucketSeries(1);
        var noise = new Random(7);
        for (var t = 0; t < 36000; t++)
            traffic.Add(t, noise.Next(0, 95000));
        traffic.Seal();
        const double length = 18000, plotWidth = 550;
        var step = PlotEnvelope.Step(length, plotWidth);
        Dictionary<long, (double Mean, int Count)> Drawn(double start)
        {
            var read = traffic.Read(start, start + length, 1200);
            var readings = read.Select(s => (s.SourceTime, new Avalonia.Point((s.SourceTime - start) / length * plotWidth, s.Value!.Value))).ToArray();
            return PlotEnvelope.Reduce(readings, step).Zip(readings.GroupBy(r => (long)Math.Floor(r.SourceTime / step)).OrderBy(g => g.Key))
                .ToDictionary(pair => pair.Second.Key, pair => (pair.First.Mean, pair.First.Count));
        }
        var first = Drawn(3600);
        var moved = Drawn(3600 + 37.3);
        var shared = first.Keys.Intersect(moved.Keys).Where(key => key * step >= 3600 + 37.3 + step && (key + 1) * step <= 3600 + length - step).ToArray();
        Check(shared.Length > 250 && shared.All(key => first[key] == moved[key]),
            "panning a 5-hour history range keeps every drawn column's mean");

        var coarse = new BucketSeries(60);
        for (var t = 0; t < 120; t++)
            coarse.Add(t, 5);
        coarse.Seal();
        Check(coarse.Read(30, 90, 600).Length > 0, "a range inside one coarse bucket still shows data");

        var unordered = new BucketSeries(1);
        unordered.Add(10, 1);
        unordered.Add(5, 2);
        unordered.Seal();
        Check(unordered.Stats(0, 20)!.Count == 2 && unordered.First == 5 && unordered.Last == 10 && unordered.Stats(0, 20)!.Last == 1,
            "out-of-order readings land in their own buckets");
        Check(unordered.Stats(100, 200) == null, "a range without readings has no stats");
        Check(unordered.Stats(0, 7)!.Last == 2, "a reading that opens an earlier bucket is that bucket's latest value");
        var sealedRejects = false;
        try { unordered.Add(1, 1); } catch (InvalidOperationException) { sealedRejects = true; }
        Check(sealedRejects, "a sealed series rejects new readings");
    }
}
