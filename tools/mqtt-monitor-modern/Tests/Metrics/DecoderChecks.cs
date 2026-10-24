using CakeAdaptMonitor;

static partial class Tests
{
    private static void DecoderChecks()
    {
        var store = new MetricStore();
        store.Accept("a", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 100; 10\nX; 100\nX; 101; 20", 101);
        var metric = store.Snapshot(60, 101).Single();
        Check(store.SchemaErrors == 1 && metric.Latest == "20" && metric.Samples.Length == 2,
            "malformed records do not prevent decoding later records in the same message");
        store.Accept("a", "X_HEADER; LOG_TIMESTAMP; REPLACEMENT\nX; -5; 30", 102);
        var replacement = store.Snapshot(60, 102).Single(item => item.Name == "REPLACEMENT");
        Check(replacement.Samples.Single().SourceTime == 102,
            "replacement schemas apply immediately and invalid timestamps use receipt time");
        Check(store.Snapshot(60, 102).Single(item => item.Name == "VALUE").Latest == "20",
            "schema replacement preserves measurements of fields no longer published");

        var headers = new MetricStore();
        for (var i = 0; i < 256; i++)
            headers.Accept($"topic-{i}", "X_HEADER; LOG_TIMESTAMP; VALUE", 100);
        headers.Accept("topic-0", "X_HEADER; LOG_TIMESTAMP; REPLACEMENT", 100);
        headers.Accept("new-topic", "X_HEADER; LOG_TIMESTAMP; VALUE", 100);
        headers.Accept("topic-0", "X; 100; 42", 100);
        headers.Accept("topic-1", "X; 100; 99", 100);
        Check(headers.HeaderCount == 256 && headers.EvictedHeaders == 1 &&
            headers.Snapshot(60, 100).Single().Name == "REPLACEMENT",
            "replacement headers refresh bounded FIFO order without stale queue entries");
        var large = new string('A', 600000);
        headers.Accept("large-a", "X_HEADER; LOG_TIMESTAMP; " + large, 100);
        headers.Accept("large-b", "X_HEADER; LOG_TIMESTAMP; " + large, 100);
        Check(headers.HeaderCount == 1, "header cache enforces its byte limit as well as its entry limit");
        headers.Accept("oversize", "X_HEADER; LOG_TIMESTAMP; " + new string('B', 1048577), 100);
        Check(headers.HeaderCount == 0, "oversize header cannot exceed the cache byte bound");

        for (var i = 0; i < 160; i++)
            store.Accept("unknown", new string('z', 2100), 200 + i);
        Check(store.Events().Length == 150 && store.Events().All(item => item.Topic == "unknown" && item.Text.Length < 2100),
            "unknown records remain available in the bounded event view");
    }
}
