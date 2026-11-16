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
        var sealedRejects = false;
        try { unordered.Add(1, 1); } catch (InvalidOperationException) { sealedRejects = true; }
        Check(sealedRejects, "a sealed series rejects new readings");
    }
}
