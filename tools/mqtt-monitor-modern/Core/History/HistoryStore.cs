namespace CakeAdaptMonitor;

// Capture records decoded once into compact series for history mode. Built by one
// loader thread, then sealed; after Seal it is only read (by the UI thread).
public sealed class HistoryStore
{
    private readonly RecordDecoder decoder = new();
    private readonly Dictionary<MetricKey, BucketSeries> series = new();
    private readonly Dictionary<MetricKey, (string Text, double Time)> texts = new();
    private readonly BloatEvents bloat = new(double.PositiveInfinity, HistoryModeDefaults.DetailedStepSeconds);
    private readonly List<HistoryEvent> events = [];
    public double Start { get; private set; } = double.PositiveInfinity;
    public double End { get; private set; } = double.NegativeInfinity;
    public long Records { get; private set; }
    public long BadLines { get; private set; }
    public long SchemaErrors => decoder.Errors;
    public string[] Topics { get; private set; } = [];

    public void CountBad() => BadLines++;

    public void Accept(string topic, string payload, double tst)
    {
        Records++;
        var decoded = 0;
        foreach (var metric in decoder.Decode(topic, payload, tst))
        {
            decoded++;
            // Router clocks can be wrong (or 0 before NTP); trust the recorder then.
            var time = Math.Abs(metric.SourceTime - tst) <= HistoryModeDefaults.ClockToleranceSeconds ? metric.SourceTime : tst;
            bloat.Record(topic, metric.Key.Record, metric.Key.Name, metric.Text, time);
            if (metric.Number is { } number)
            {
                if (!series.TryGetValue(metric.Key, out var data))
                    series[metric.Key] = data = new BucketSeries(ReportCatalog.IsDetailed(metric.Key.Name)
                        ? HistoryModeDefaults.DetailedStepSeconds : HistoryModeDefaults.CoarseStepSeconds);
                data.Add(time, number);
            }
            else if (!texts.TryGetValue(metric.Key, out var text) || time >= text.Time)
                texts[metric.Key] = (metric.Text, time);
            Start = Math.Min(Start, time);
            End = Math.Max(End, time);
        }
        var kind = payload.Split(ProtocolNames.CellSeparator, 2)[0].Trim();
        if (decoded == 0 && !kind.EndsWith(ProtocolNames.HeaderSuffix) && double.IsFinite(tst))
        {
            events.Add(new HistoryEvent(tst, topic, payload));
            Start = Math.Min(Start, tst);
            End = Math.Max(End, tst);
        }
    }

    public void Seal()
    {
        foreach (var data in series.Values)
            data.Seal();
        events.Sort((a, b) => a.Time.CompareTo(b.Time));
        Topics = series.Keys.Select(key => key.Topic).Concat(texts.Keys.Select(key => key.Topic))
            .Distinct().Order(StringComparer.Ordinal).ToArray();
    }

    public MetricView[] Snapshot(double start, double end, int columns)
    {
        var views = new List<MetricView>();
        foreach (var (key, data) in series)
        {
            if (data.Stats(start, end) is not { } stats)
                continue;
            var traffic = key.Name is MetricNames.DownloadTraffic or MetricNames.UploadTraffic;
            views.Add(new MetricView(key, Telemetry.Format(stats.Last), stats.LastTime, data.Read(start, end, columns))
            {
                Stats = stats,
                RatePeak = traffic ? new Sample(stats.MaxTime, stats.MaxTime, stats.Max) : null
            });
        }
        foreach (var (key, text) in texts)
            if (text.Time <= end)
                views.Add(new MetricView(key, text.Text, text.Time, []));
        return views.ToArray();
    }

    public BloatView[] Bufferbloat(double start, double end) => bloat.Read(end - start, end);

    public RecentEvent[] Events(double start, double end) => events
        .Where(item => item.Time >= start && item.Time <= end)
        .Select(item => new RecentEvent(item.Topic, DateTimeOffset.FromUnixTimeMilliseconds((long)(item.Time * TelemetryDefaults.MillisecondsPerSecond))
            .ToLocalTime().ToString(TelemetryDefaults.ClockFormat) + "  " + item.Topic + "  " + item.Text))
        .ToArray();

    // Download traffic per source over the whole history, preferring the global
    // LOAD/SUMMARY series over per-reflector DATA.
    public (string Topic, Sample[] Points)[] Overview(int columns) => Topics
        .Select(topic => series.Where(pair => pair.Key.Topic == topic && pair.Key.Name == MetricNames.DownloadTraffic)
            .OrderByDescending(pair => pair.Key.Dimension.Length == 0)
            .ThenByDescending(pair => pair.Key.Record is ProtocolNames.LoadRecord or ProtocolNames.SummaryRecord)
            .Select(pair => (topic, pair.Value.Read(Start, End, columns))).FirstOrDefault())
        .Where(item => item.topic != null)
        .Select(item => (item.topic, item.Item2))
        .ToArray();
}
