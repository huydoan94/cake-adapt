namespace CakeAdaptMonitor;

public sealed record BloatBucket(double Time, int Count);
public sealed record BloatView(string Topic, string Direction, int Count, BloatBucket[] Buckets);

// Owned by MetricStore and accessed only under its lock. Counts samples whose
// load condition ends in "_bb" (the router measured delay above its threshold),
// per topic, record and direction, in fixed buckets by router time for one hour.
// DATA carries one condition per probe; SUMMARY repeats it, so a topic that
// publishes DATA is counted from DATA only.
public sealed class BloatEvents
{
    private sealed class Track
    {
        public readonly SortedList<long, int> Buckets = new();
        public double Latest = double.NegativeInfinity;
    }
    private readonly Dictionary<(string Topic, string Record, string Direction), Track> tracks = new();

    public void Record(string topic, string record, string name, string text, double sourceTime)
    {
        var direction = name switch
        {
            ProtocolNames.DownloadCondition => ProtocolNames.Download,
            ProtocolNames.UploadCondition => ProtocolNames.Upload,
            _ => null
        };
        if (direction == null || !double.IsFinite(sourceTime))
            return;
        var key = (topic, record, direction);
        if (!tracks.TryGetValue(key, out var track))
        {
            if (tracks.Count >= TelemetryDefaults.MaximumBloatTracks)
                tracks.Remove(tracks.MinBy(pair => pair.Value.Latest).Key);
            tracks[key] = track = new Track();
        }
        track.Latest = Math.Max(track.Latest, sourceTime);
        if (text.EndsWith(ProtocolNames.BloatSuffix, StringComparison.Ordinal))
        {
            var bucket = (long)Math.Floor(sourceTime / HistoryDefaults.BloatStepSeconds);
            track.Buckets[bucket] = track.Buckets.GetValueOrDefault(bucket) + 1;
        }
        while (track.Buckets.Count > 0 &&
               (track.Buckets.Keys[0] + 1) * HistoryDefaults.BloatStepSeconds < track.Latest - HistoryDefaults.RetentionSeconds)
            track.Buckets.RemoveAt(0);
    }

    public BloatView[] Read(double seconds, double now)
    {
        var start = now - Math.Min(seconds, HistoryDefaults.RetentionSeconds);
        return tracks.GroupBy(pair => pair.Key.Topic)
            .SelectMany(topic =>
            {
                var record = topic.Any(pair => pair.Key.Record == ProtocolNames.DataRecord)
                    ? ProtocolNames.DataRecord : topic.Min(pair => pair.Key.Record)!;
                return topic.Where(pair => pair.Key.Record == record).Select(pair =>
                {
                    var buckets = pair.Value.Buckets
                        .Where(bucket => (bucket.Key + 1) * HistoryDefaults.BloatStepSeconds > start &&
                                         bucket.Key * HistoryDefaults.BloatStepSeconds <= now)
                        .Select(bucket => new BloatBucket(bucket.Key * HistoryDefaults.BloatStepSeconds, bucket.Value))
                        .ToArray();
                    return new BloatView(pair.Key.Topic, pair.Key.Direction, buckets.Sum(bucket => bucket.Count), buckets);
                });
            })
            .OrderBy(view => view.Topic, StringComparer.Ordinal).ThenBy(view => view.Direction, StringComparer.Ordinal)
            .ToArray();
    }
}
