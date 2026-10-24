namespace CakeAdaptMonitor;

// Keep a full hour by receive time. Each bucket retains actual first/last,
// minimum/maximum and an invalid reading rather than averaging away spikes.
public sealed class MetricHistory
{
    private readonly SortedDictionary<long, Bucket> recent = new();
    private readonly SortedDictionary<long, Bucket> older = new();
    private double lastReceived = double.NegativeInfinity;
    public int BucketCount => recent.Count + older.Count;

    private sealed class Bucket
    {
        private Sample? first, last, minimum, maximum, invalid;
        public void Add(Sample sample)
        {
            if (first == null || sample.ReceivedTime < first.ReceivedTime)
                first = sample;
            if (last == null || sample.ReceivedTime >= last.ReceivedTime)
                last = sample;
            if (sample.Value.HasValue)
            {
                if (minimum == null || sample.Value < minimum.Value)
                    minimum = sample;
                if (maximum == null || sample.Value > maximum.Value)
                    maximum = sample;
            }
            else
                invalid ??= sample;
        }
        public IEnumerable<Sample> Points() => new[] { first, minimum, maximum, invalid, last }
            .OfType<Sample>().Distinct();
    }

    public void Add(Sample sample)
    {
        if (!double.IsFinite(sample.ReceivedTime))
            return;
        if (sample.ReceivedTime < lastReceived - HistoryDefaults.ClockResetSeconds)
        {
            // A backwards PC clock correction starts a new receive-time history.
            recent.Clear();
            older.Clear();
        }
        lastReceived = sample.ReceivedTime;
        Trim(sample.ReceivedTime);
        var key = (long)Math.Floor(sample.ReceivedTime / HistoryDefaults.RecentStepSeconds);
        if (!recent.TryGetValue(key, out var bucket))
            recent[key] = bucket = new Bucket();
        bucket.Add(sample);
    }

    private void Trim(double now)
    {
        while (recent.Count > 0 && (recent.First().Key + 1) * HistoryDefaults.RecentStepSeconds <= now - HistoryDefaults.DetailedSeconds)
        {
            var old = recent.First();
            recent.Remove(old.Key);
            foreach (var point in old.Value.Points())
            {
                var key = (long)Math.Floor(point.ReceivedTime / HistoryDefaults.OlderStepSeconds);
                if (!older.TryGetValue(key, out var bucket))
                    older[key] = bucket = new Bucket();
                bucket.Add(point);
            }
        }
        while (older.Count > 0 && (older.First().Key + 1) * HistoryDefaults.OlderStepSeconds < now - HistoryDefaults.RetentionSeconds)
            older.Remove(older.First().Key);
    }

    public Sample[] Read(double seconds, double now)
    {
        Trim(now);
        var start = now - Math.Min(seconds, HistoryDefaults.RetentionSeconds);
        return older.Values.Concat(recent.Values).SelectMany(b => b.Points())
            .Where(p => p.ReceivedTime >= start && p.ReceivedTime <= now)
            .OrderBy(p => p.SourceTime).ToArray();
    }
}
