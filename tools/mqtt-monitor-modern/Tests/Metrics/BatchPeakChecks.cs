using CakeAdaptMonitor;

static partial class Tests
{
    private static void BatchPeakChecks()
    {
        var store = new MetricStore();
        void Accept(string topic, string record, string cpu, string value, string state, double at,
            string valid = "1", string queue = "900")
        {
            store.Accept(topic, $"{record}_HEADER; LOG_TIMESTAMP; CPU_ID; VALUE; STATE; DL_QUEUE_VALID; DL_QUEUE_US", at);
            store.Accept(topic, $"{record}; {at:R}; {cpu}; {value}; {state}; {valid}; {queue}", at);
        }
        MetricView Value(string topic = "a", string record = "X", string cpu = "0") =>
            store.Snapshot(3600, 300).Single(m => m.Topic == topic && m.Record == record && m.Dimension == cpu && m.Name == "VALUE");
        Accept("a", "X", "0", "123.4567890123456789", "warmup", 100);
        Accept("a", "X", "0", "2", "idle", 101, "0", "2000");
        store.PublishBatch();
        var peak = Value();
        Check(new MetricRow(peak).Latest == "123.4567890123456789" && peak.Latest == "2",
            "All metrics batch peak preserves exact original text while raw latest remains available");
        Check(peak.BatchPeak?.ReceivedTime == 100,
            "All metrics peak keeps the receipt time of the winning reading");
        Check(store.Snapshot(3600, 300).Single(m => m.Name == "STATE").BatchPeak?.Text == "idle",
            "text fields use the last text in each batch");
        Check(store.Snapshot(3600, 300).Single(m => m.Name == "DL_QUEUE_US").BatchPeak?.Text == "900",
            "invalid trailing queue does not erase a valid peak in the same batch");
        Accept("a", "X", "0", "80", "busy", 102);
        Accept("a", "X", "0", "3", "idle", 103);
        Check(new MetricRow(Value()).Latest == "123.4567890123456789",
            "ordinary snapshots cannot consume or replace the displayed batch");
        store.PublishBatch();
        Check(new MetricRow(Value()).Latest == "80", "the next batch can have a lower numeric peak");
        var before = Value().BatchPeak;
        store.PublishBatch();
        Check(Value().BatchPeak == before, "no-data batch retains its previous reading and age");
        Accept("a", "X", "1", "11", "idle", 104);
        Accept("a", "Y", "0", "33", "idle", 104);
        Accept("b", "X", "0", "44", "idle", 104);
        store.PublishBatch();
        Check(Value().BatchPeak?.Text == "80" && Value(cpu: "1").BatchPeak?.Text == "11" &&
            Value(record: "Y").BatchPeak?.Text == "33" && Value(topic: "b").BatchPeak?.Text == "44",
            "batch peaks remain separate by topic, record, CPU or reflector, and field");
        Accept("a", "X", "0", "NaN", "failed", 105, "0");
        store.PublishBatch();
        Check(new MetricRow(Value()).Latest == "NaN" &&
            store.Snapshot(3600, 300).Single(m => m.Topic == "a" && m.Record == "X" && m.Dimension == "0" && m.Name == "DL_QUEUE_US").BatchPeak?.Text == "Unavailable",
            "invalid-only batch reports invalid data instead of a stale valid maximum");
        Accept("a", "X", "0", "-12", "idle", 106);
        Accept("a", "X", "0", "-8", "idle", 107);
        Accept("a", "X", "0", "-10", "idle", 108);
        store.PublishBatch();
        Check(new MetricRow(Value()).Latest == "-8", "negative-valued metrics select the numeric maximum");
        Accept("a", "X", "0", "0", "idle", 109);
        store.PublishBatch();
        Check(new MetricRow(Value()).Latest == "0", "zero and counter resets appear in the next batch");
        Check(Value().Samples.Any(p => p.Value == 80) && Value().Samples.Any(p => p.Value == 0),
            "batch display does not replace retained chart/history samples");
        var bounded = new MetricStore { MaxSeries = 2 };
        foreach (var topic in new[] { "a", "b", "c" })
            bounded.Accept(topic, "V_HEADER; LOG_TIMESTAMP; VALUE\nV; 100; 42", 100);
        bounded.PublishBatch();
        Check(bounded.SeriesCount == 2 && bounded.Snapshot(60, 100).All(m => m.Topic != "a" && m.BatchPeak?.Text == "42"),
            "batch peaks are owned by the existing bounded series cache");
    }
}
