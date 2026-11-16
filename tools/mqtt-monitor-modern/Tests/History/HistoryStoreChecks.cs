using CakeAdaptMonitor;

static partial class Tests
{
    // 120 s of LOAD and DATA for topics "a" and "b"; three DATA lines of "b" flag
    // download bufferbloat; one INFO line; one DATA line with LOG_TIMESTAMP 0.
    internal static HistoryStore HistoryFixture(double start = 1_800_000_000)
    {
        var store = new HistoryStore();
        foreach (var topic in new[] { "a", "b" })
            store.Accept(topic, "DATA_HEADER; LOG_TIMESTAMP; REFLECTOR; DL_AVG_OWD_DELTA_US; DL_OWD_BASELINE; DL_LOAD_CONDITION; UL_LOAD_CONDITION", start);
        for (var i = 0; i < 120; i++)
        {
            var t = start + i;
            foreach (var topic in new[] { "a", "b" })
            {
                var download = topic == "a" ? 1000 + i : 2000 + i;
                store.Accept(topic, $"LOAD; date; {t:R}; 100; {download}; 500; 100000; 20000", t);
                var bloat = topic == "b" && i is 10 or 11 or 12 ? "dl_high_bb" : "dl_low";
                store.Accept(topic, $"DATA; {t:R}; 1.1.1.1; {3000 + i}; 7000; {bloat}; ul_low", t);
            }
        }
        store.Accept("a", "INFO; date; 1; line load changed: direction=download", start + 60);
        store.Accept("a", "DATA; 0; 9.9.9.9; 5000; 7000; dl_low; ul_low", start + 30);
        store.Seal();
        return store;
    }

    private static void HistoryStoreChecks()
    {
        // A field that is a number in some readings and text in others (an invalid
        // queue reading becomes "Unavailable") must appear once in a snapshot.
        var mixed = new HistoryStore();
        mixed.Accept("q", "X_HEADER; LOG_TIMESTAMP; VALUE", 1_800_000_000);
        mixed.Accept("q", "X; 1800000000; 100", 1_800_000_000);
        mixed.Accept("q", "X; 1800000001; Unavailable", 1_800_000_001);
        mixed.Accept("q", "X; 1800000002; 120", 1_800_000_002);
        mixed.Seal();
        var keys = mixed.Snapshot(mixed.Start, mixed.End).Select(v => v.Key).ToArray();
        Check(keys.Length == keys.Distinct().Count() && keys.Length == 1, "a field with number and text readings appears once in a history snapshot");
        const double start = 1_800_000_000;
        var store = HistoryFixture(start);
        var views = store.Snapshot(store.Start, store.End);
        MetricView View(string topic, string name, string dimension = "") =>
            views.First(v => v.Topic == topic && v.Name == name && (dimension.Length == 0 || v.Dimension == dimension));
        Check(View("a", MetricNames.DownloadTraffic).Stats!.Count == 120 && View("b", MetricNames.DownloadTraffic).Stats!.Count == 120,
            "history keeps every reading of a field per source");
        var peak = View("b", MetricNames.DownloadTraffic).RatePeak!;
        Check(peak.Value == 2119 && peak.ReceivedTime == start + 119, "history cards get the range peak and its time");
        Check(View("a", MetricNames.DownloadTraffic).Max == "1119" && View("a", MetricNames.DownloadTraffic).Count == 120,
            "table statistics come from exact range stats");
        var bloat = store.Bufferbloat(store.Start, store.End);
        Check(bloat.Single(v => v.Topic == "b" && v.Direction == "DL").Count == 3 &&
              bloat.Single(v => v.Topic == "a" && v.Direction == "DL").Count == 0,
            "history counts bufferbloat samples per source");
        Check(store.Events(store.Start, store.End).Any(e => e.Text.Contains("line load changed")) &&
              store.Events(start + 61, store.End).Length == 0,
            "history keeps text events with their times");
        Check(store.Start >= start, "a router time far outside the capture day falls back to the receive time");
        Check(View("a", MetricNames.DownloadDelay, "9.9.9.9").Stats!.LastTime == start + 30,
            "a fallback reading is placed at its receive time");
        Check(views.All(v => v.Samples.Length == 0), "a history snapshot carries statistics only; chart points are read on demand");
        Check(store.Samples(View("a", "DL_OWD_BASELINE", "1.1.1.1").Key, store.Start, store.End, 600).Length <= 4 &&
              store.Samples(View("a", MetricNames.DownloadDelay, "1.1.1.1").Key, store.Start, store.End, 600).Length > 100,
            "chart fields keep one-second detail and other fields one-minute detail");
        var middle = store.Snapshot(start + 30, start + 89);
        Check(middle.First(v => v.Topic == "a" && v.Name == MetricNames.DownloadTraffic).Stats!.Count == 60,
            "a snapshot covers only the chosen range");
        Check(store.Topics.SequenceEqual(["a", "b"]) && store.Overview(100).Select(o => o.Topic).SequenceEqual(["a", "b"]) &&
              store.Overview(100).All(o => o.Points.Length > 0),
            "the overview has a download trace per source");
    }
}
