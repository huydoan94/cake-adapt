using CakeAdaptMonitor;

static partial class Tests
{
    private static void MetricStoreChecks()
    {
        var store = new MetricStore { MaxSeries = 6 };
        store.Accept("cake-adapt/a", "LOAD; date; 100; 1; 1000; 2000; 3000; 4000", 100);
        Check(store.Snapshot(60, 100).Single(m => m.Name == "DL_ACHIEVED_RATE_KBPS").Latest == "1000", "current schema without startup header");
        store.Accept("cake-adapt/a", "FUTURE_HEADER; LOG_TIMESTAMP; NEW_US\nFUTURE; 100; 42", 100);
        Check(store.Snapshot(60, 100).Single(m => m.Name == "NEW_US").Latest == "42", "dynamic headers");
        var separate = new MetricStore();
        separate.Accept("a", "CUSTOM_HEADER; LOG_TIMESTAMP; VALUE", 100);
        separate.Accept("b", "CUSTOM; 100; 99", 100);
        Check(separate.SeriesCount == 0, "headers scoped to topic");
        separate.Accept("a", "CPU; date; 100; 1; 25; 10; 40", 100);
        Check(separate.Snapshot(60, 100).Single(m => m.Name == "CPU1_USAGE").Latest == "40", "dynamic CPU fallback");
        separate.Accept("a", "TCP_QUEUE; date; 100; 1; 0; 999; 1; 20", 100);
        Check(separate.Snapshot(60, 100).Single(m => m.Name == "DL_QUEUE_US").Samples[0].Value == null, "invalid queues produce gaps");
        for (var i = 0; i < 1000; i++)
            separate.Accept("header" + i, "X_HEADER; LOG_TIMESTAMP; VALUE", 100);
        Check(separate.HeaderCount <= 256 && separate.EvictedHeaders > 0, "header cache bounded");
        for (var i = 0; i < 1000; i++)
            separate.Accept("header999", "X_HEADER; LOG_TIMESTAMP; VALUE", 100);
        Check(separate.HeaderCount <= 256, "repeated headers do not grow cache");
        var bounded = new MetricStore { MaxSeries = 2 };
        for (var i = 0; i < 10; i++)
            bounded.Accept("a", $"X_HEADER; LOG_TIMESTAMP; VALUE\nX; {i}; {i}", i);
        Check(bounded.Snapshot(60, 9).Single().Samples.Length == 10, "available short history retained");
        Check(bounded.Snapshot(.5, 9).Single().Samples.Length == 1, "receive-time window");
        bounded.Accept("b", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 9; 8", 9);
        bounded.Accept("c", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 9; 8", 9);
        Check(bounded.SeriesCount == 2 && bounded.EvictedSeries == 1, "series count bounded");
        Check(Telemetry.Number("nan") == null && Telemetry.Number("1e999") == null, "nonfinite payloads excluded");
        Check(double.IsFinite(Telemetry.Mean([1.79e308, 1.79e308])) && Telemetry.Mean([-1e308, 1e308]) == 0, "finite extreme means");
        var invalid = new MetricStore();
        invalid.Accept("a", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 100; 1\nX; 101; NaN\nX; 102; 3", 100);
        Check(invalid.Snapshot(60, 100).Single().Samples[1].Value == null, "invalid numeric updates create chart gaps");
        var key = new MetricKey("a", "SUMMARY", "", "DL_ACHIEVED_RATE_KBPS");
        var stale = new MetricView(key, "1", 1, []);
        var fresh = new MetricView(key with
        {
            Record = "LOAD"
        }, "5", 100, [new Sample(100, 100, 5)]);
        Check(ReportCatalog.SelectSeries([stale, fresh]).Single() == fresh, "live LOAD replaces stale SUMMARY");
    }

}
