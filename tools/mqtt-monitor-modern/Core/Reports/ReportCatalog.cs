namespace CakeAdaptMonitor;

public static class ReportCatalog
{
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
