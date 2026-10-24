namespace CakeAdaptMonitor;

// One synchronization boundary for decoding, stored metrics and immutable snapshots.
public sealed class MetricStore
{
    private readonly object gate = new();
    private readonly RecordDecoder decoder = new();
    private readonly RatePeaks ratePeaks = new();
    private readonly Dictionary<MetricKey, MetricSeries> series = new();
    private readonly Queue<RecentEvent> events = new();
    private long order;
    private long evictedSeries;
    public int MaxSeries { get; init; } = TelemetryDefaults.MaximumSeries;
    public long SchemaErrors
    {
        get
        {
            lock (gate)
                return decoder.Errors;
        }
    }
    public long EvictedSeries
    {
        get
        {
            lock (gate)
                return evictedSeries;
        }
    }
    public long EvictedHeaders
    {
        get
        {
            lock (gate)
                return decoder.Schemas.Evicted;
        }
    }
    public int HeaderCount
    {
        get
        {
            lock (gate)
                return decoder.Schemas.Count;
        }
    }
    public int SeriesCount
    {
        get
        {
            lock (gate)
                return series.Count;
        }
    }

    public void Accept(string topic, string payload, double received)
    {
        lock (gate)
        {
            RememberEvent(topic, payload, received);
            foreach (var metric in decoder.Decode(topic, payload, received))
            {
                var data = GetSeries(metric.Key);
                var sample = data.Add(metric, received, ++order);
                if (sample != null)
                    ratePeaks.Record(topic, metric.Key.Name, sample);
            }
        }
    }

    private MetricSeries GetSeries(MetricKey key)
    {
        if (series.TryGetValue(key, out var data))
            return data;
        if (series.Count >= MaxSeries)
        {
            series.Remove(series.MinBy(pair => pair.Value.Order).Key);
            evictedSeries++;
        }
        data = new MetricSeries();
        series.Add(key, data);
        return data;
    }

    private void RememberEvent(string topic, string payload, double received)
    {
        var clock = DateTimeOffset.FromUnixTimeSeconds((long)received).ToLocalTime().ToString(TelemetryDefaults.ClockFormat);
        var text = payload[..Math.Min(TelemetryDefaults.EventCharacters, payload.Length)];
        events.Enqueue(new RecentEvent(topic, clock + "  " + topic + "  " + text));
        while (events.Count > TelemetryDefaults.MaximumEvents)
            events.Dequeue();
    }

    public MetricView[] Snapshot(double seconds, double? at = null)
    {
        var now = at ?? Telemetry.Now;
        lock (gate)
            return series.Select(pair => pair.Value.Snapshot(pair.Key, seconds, now,
                ratePeaks.Read(pair.Key.Topic, pair.Key.Name))).ToArray();
    }

    // Advance only on the screen timer, not on source/search/settings refreshes.
    public void PublishBatch()
    {
        lock (gate)
        {
            foreach (var data in series.Values)
                data.PublishBatch();
            ratePeaks.Publish();
        }
    }

    public RecentEvent[] Events()
    {
        lock (gate)
            return events.ToArray();
    }
}
