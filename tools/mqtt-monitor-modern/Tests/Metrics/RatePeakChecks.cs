using CakeAdaptMonitor;
static partial class Tests
{
    private static void RatePeakChecks()
    {
        var store = new MetricStore();
        store.Accept("a", "LOAD; date; 100; 1; 90000; 7000; 120000; 30000", 100);
        store.Accept("a", "LOAD; date; 100.1; 1; 40; 30; 125000; 31000", 100.1);
        store.PublishBatch();
        var dl = store.Snapshot(60, 100.2).Single(m => m.Name == "DL_ACHIEVED_RATE_KBPS");
        Check(dl.RatePeak?.Value == 90000 && dl.Latest == "40", "cards keep the interval peak while raw latest keeps the last value");
        Check(dl.RatePeak!.ReceivedTime == 100, "card peak age belongs to the peak reading");
        var rate = store.Snapshot(60, 100.2).Single(m => m.Name == "CAKE_DL_RATE_KBPS");
        Check(rate.RatePeak == null && rate.Latest == "125000", "CAKE settings retain current configured rates");
        store.Accept("a", "COPY_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_ACHIEVED_RATE_KBPS\nCOPY; 100.3; reflector; 1", 100.3);
        Check(store.Snapshot(60, 100.3).Where(m => m.Name == "DL_ACHIEVED_RATE_KBPS").All(m => m.RatePeak?.Value == 90000),
            "source and settings refreshes do not consume or replace the displayed peak");
        store.Accept("a", "LOAD; date; 100.4; 1; 300; NaN; 125000; 31000", 100.4);
        store.PublishBatch();
        Check(store.Snapshot(60, 100.4).Where(m => m.Name == "DL_ACHIEVED_RATE_KBPS").All(m => m.RatePeak?.Value == 300),
            "next interval can show a lower peak across global and reflector records");
        Check(store.Snapshot(60, 100.4).Single(m => m.Name == "UL_ACHIEVED_RATE_KBPS").RatePeak?.Value == 7000,
            "invalid-only intervals retain the last valid peak and its age");
        store.Accept("b", "LOAD; date; 100.5; 1; 80000; 6000; 125000; 31000", 100.5);
        store.PublishBatch();
        Check(store.Snapshot(60, 100.5).Single(m => m.Topic == "b" && m.Name == "DL_ACHIEVED_RATE_KBPS").RatePeak?.Value == 80000 &&
            store.Snapshot(60, 100.5).First(m => m.Topic == "a" && m.Name == "DL_ACHIEVED_RATE_KBPS").RatePeak?.Value == 300,
            "traffic peaks remain isolated by topic and direction");
        var peaks = new RatePeaks(2);
        peaks.Record("a", "DL_ACHIEVED_RATE_KBPS", new(1, 1, 10));
        peaks.Record("b", "DL_ACHIEVED_RATE_KBPS", new(2, 2, 20));
        peaks.Record("c", "DL_ACHIEVED_RATE_KBPS", new(3, 3, 30));
        Check(peaks.Read("a", "DL_ACHIEVED_RATE_KBPS") == null && peaks.Read("c", "DL_ACHIEVED_RATE_KBPS")?.Value == 30,
            "traffic peak cache evicts the oldest source when full");
    }
}
