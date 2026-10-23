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
        var metrics = name == ReportNames.SelectedMetric ? all.Where(m => m.Key == selected) :
            name == ReportNames.Cpu ? all.Where(m => m.Name.EndsWith(ProtocolNames.UsageSuffix)) : all.Where(m => fields.Contains(m.Name));
        // Global LOAD/SUMMARY fields also appear in per-reflector DATA records.
        // Prefer the global series when available, while keeping genuinely
        // dimensional measurements (or a DATA-only source) accessible.
        return Telemetry.SelectSeries(metrics).GroupBy(m => (m.Topic, m.Name))
            .SelectMany(g => g.Any(m => m.Dimension.Length == 0)
                ? g.Where(m => m.Dimension.Length == 0) : g);
    }
}
