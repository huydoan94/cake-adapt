using CakeAdaptMonitor;

static partial class Tests
{
    private static void DefaultTraceChecks()
    {
        MetricView Trace(string reflector, double received, string field = "DL_AVG_OWD_DELTA_US",
            string topic = "a") => new(new MetricKey(topic, "DATA", reflector, field), "10", received,
                [new Sample(received, received, 10)]);
        var old = Enumerable.Range(0, 6).Select(i => Trace("old-" + i, 100)).ToArray();
        var fresh = Enumerable.Range(0, 6).Select(i => Trace("new-" + i, 200 + i)).ToArray();
        var candidates = old.Concat(fresh).ToArray();
        Check(ReportCatalog.Defaults(candidates).SequenceEqual(fresh),
            "automatic defaults replace six stale reflectors with six recently updated traces");
        var updated = candidates.Select(metric => metric.Key == fresh[0].Key
            ? metric with { ReceivedTime = 300 } : metric).ToArray();
        Check(ReportCatalog.Defaults(updated).Select(metric => metric.Key).SequenceEqual(fresh.Select(metric => metric.Key)),
            "probe arrival order does not reshuffle the active legend");
        var replacement = Trace("replacement", 400);
        Check(ReportCatalog.Defaults(updated.Append(replacement).ToArray()).Select(metric => metric.Dimension)
            .ToHashSet().SetEquals(["new-0", "new-2", "new-3", "new-4", "new-5", "replacement"]),
            "a later reflector rotation replaces the least recently updated default trace");
        var upload = Trace("upload", 500, "UL_AVG_OWD_DELTA_US");
        Check(ReportCatalog.Defaults(fresh.Append(upload).ToArray()).SequenceEqual(fresh),
            "default rotation keeps the existing report field priority");
        Check(ReportCatalog.Defaults(fresh.Take(2).ToArray()).Length == 2 &&
            ReportCatalog.Defaults([]).Length == 0,
            "defaults handle fewer than six available traces and no data");
        var other = fresh.Select(metric => metric with { Key = metric.Key with { Topic = "b" } }).ToArray();
        Check(ReportCatalog.Defaults(other).All(metric => metric.Topic == "b"),
            "automatic defaults respect the supplied topic scope");
    }
}
