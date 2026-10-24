namespace CakeAdaptMonitor;

public static class ReportNames
{
    public const string Throughput = "Throughput", Latency = "Latency", Cpu = "CPU", Memory = "Memory";
    public const string TcpQueues = "TCP queues", TcpCounters = "TCP counters", SelectedMetric = "Selected metric";
    public static readonly string[] All = [Throughput, Latency, Cpu, Memory, TcpQueues, TcpCounters, SelectedMetric];
    public static readonly string[] Initial = [Throughput, Latency, Cpu, Memory];
}
public static class ChartDefaults
{
    public const int AutomaticTraces = 6;
    public const int MaximumTraces = 24;
    public const double DefaultWindowSeconds = 300;
    public const string AllSources = "All sources";
    public static readonly double[] WindowSeconds = [60, 300, 900, 3600];
    public static readonly string[] WindowLabels = ["Last minute", "Last 5 minutes", "Last 15 minutes", "Last hour"];
    public const int DefaultWindowIndex = 1;
    public static double Window(int index) => index >= 0 && index < WindowSeconds.Length ? WindowSeconds[index] : DefaultWindowSeconds;
}
public static class MetricNames
{
    public const string DownloadTraffic = "DL_ACHIEVED_RATE_KBPS", UploadTraffic = "UL_ACHIEVED_RATE_KBPS";
    public const string DownloadRate = "CAKE_DL_RATE_KBPS", UploadRate = "CAKE_UL_RATE_KBPS";
    public const string DownloadDelay = "DL_AVG_OWD_DELTA_US", UploadDelay = "UL_AVG_OWD_DELTA_US";
    public const string Rss = "RSS_KB", PeakRss = "PEAK_RSS_KB", AnonymousRss = "RSS_ANON_KB", DataMemory = "DATA_KB";
    public const string DownloadQueue = "DL_QUEUE_US", UploadQueue = "UL_QUEUE_US";
    public const string Injected = "INJECTED", Skipped = "SKIPPED", Accepted = "ACCEPTED", Stalled = "STALLED";
    public static readonly string[] Rates = [DownloadTraffic, UploadTraffic, DownloadRate, UploadRate];
}
public static class ReportUnits
{
    public const string Rate = "Mbit/s", Delay = "ms", Memory = "MiB", Cpu = "%", Count = "count";
    public const double RateDivisor = 1000, DelayDivisor = 1000, MemoryDivisor = 1024;
}
