namespace CakeAdaptMonitor;

public static class ReportCatalog
{
    // Fields drawn by the dashboard reports; history keeps these at one-second detail.
    private static readonly HashSet<string> DetailedNames =
    [
        .. MetricNames.Rates, MetricNames.DownloadDelay, MetricNames.UploadDelay,
        MetricNames.Rss, MetricNames.PeakRss, MetricNames.AnonymousRss, MetricNames.DataMemory,
        MetricNames.DownloadQueue, MetricNames.UploadQueue,
        MetricNames.Injected, MetricNames.Skipped, MetricNames.Accepted, MetricNames.Stalled
    ];
    public static bool IsDetailed(string name) => DetailedNames.Contains(name) || name.EndsWith(ProtocolNames.UsageSuffix);

    public static IEnumerable<MetricView> Candidates(string name, MetricView[] all, MetricKey? selected)
    {
        var fields = name switch
        {
            ReportNames.Throughput => MetricNames.Rates,
            ReportNames.Latency => [MetricNames.DownloadDelay, MetricNames.UploadDelay],
            ReportNames.Memory => [MetricNames.Rss, MetricNames.PeakRss, MetricNames.AnonymousRss, MetricNames.DataMemory],
            ReportNames.TcpQueues => [MetricNames.DownloadQueue, MetricNames.UploadQueue],
            ReportNames.TcpCounters => [MetricNames.Injected, MetricNames.Skipped, MetricNames.Accepted, MetricNames.Stalled],
            _ => Array.Empty<string>()
        };
        var metrics = name switch
        {
            ReportNames.SelectedMetric => all.Where(metric => metric.Key == selected),
            ReportNames.Cpu => all.Where(metric => metric.Name.EndsWith(ProtocolNames.UsageSuffix)),
            _ => all.Where(metric => fields.Contains(metric.Name))
        };
        // Global LOAD/SUMMARY fields also appear in per-reflector DATA records.
        // Prefer the global series when available, while keeping genuinely
        // dimensional measurements (or a DATA-only source) accessible.
        return SelectSeries(metrics).GroupBy(m => (m.Topic, m.Name))
            .SelectMany(g => g.Any(m => m.Dimension.Length == 0)
                ? g.Where(m => m.Dimension.Length == 0) : g);
    }
    // Keep field priority within each topic, but replace rotated reflectors with
    // the most recently received series. Topics take turns, so with several
    // sources each one gets a share of the automatic traces instead of the first
    // topic filling them all. Restore candidate order to avoid reshuffling the
    // legend as individual probes arrive.
    // rank: history passes readings in the range; live uses the most recent arrival.
    public static MetricView[] Defaults(MetricView[] candidates, Func<MetricView, double>? rank = null)
    {
        var topics = candidates.GroupBy(metric => metric.Topic)
            .Select(topic => topic.GroupBy(metric => metric.Name)
                .SelectMany(field => field.OrderByDescending(rank ?? (metric => metric.ReceivedTime)))
                .Select(metric => metric.Key).ToArray())
            .ToArray();
        var active = new HashSet<MetricKey>();
        for (var position = 0; active.Count < ChartDefaults.AutomaticTraces && topics.Any(keys => position < keys.Length); position++)
            foreach (var keys in topics.Where(keys => position < keys.Length).TakeWhile(_ => active.Count < ChartDefaults.AutomaticTraces))
                active.Add(keys[position]);
        return candidates.Where(metric => active.Contains(metric.Key)).ToArray();
    }

    // Prefer current, numeric series when several records publish the same field.
    public static IEnumerable<MetricView> SelectSeries(IEnumerable<MetricView> metrics) =>
        metrics.GroupBy(metric => (metric.Topic, metric.Dimension, metric.Name)).Select(group => group
            .OrderByDescending(metric => metric.Samples.Length > 0)
            .ThenByDescending(metric => metric.ReceivedTime)
            .ThenByDescending(metric => RecordPreference(metric.Record)).First());

    private static int RecordPreference(string record) => record switch
    {
        ProtocolNames.SummaryRecord => ProtocolNames.SummaryPreference,
        ProtocolNames.LoadRecord => ProtocolNames.LoadPreference,
        _ => ProtocolNames.OtherPreference
    };
}
