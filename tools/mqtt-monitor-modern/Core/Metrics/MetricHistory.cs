namespace CakeAdaptMonitor;

// Keep a full hour by receive time. Each bucket retains actual first/last,
// minimum/maximum and an invalid reading rather than averaging away spikes.
public sealed class MetricHistory
{
    // Sorted by key in arrays: buckets arrive in time order, and a read finds its
    // first bucket by binary search instead of walking the retained hour.
    private readonly SortedList<long, Bucket> recent = new();
    private readonly SortedList<long, Bucket> older = new();
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
        public IEnumerable<Sample> Points()
        {
            var points = new List<Sample>(5);
            AddPoints(points, double.NegativeInfinity, double.PositiveInfinity);
            return points;
        }

        // Append this bucket's distinct representative readings received in [start, end].
        // Same equality as Distinct(), without a hash set for at most five samples.
        public void AddPoints(List<Sample> output, double start, double end)
        {
            var bucketStart = output.Count;
            foreach (var sample in (ReadOnlySpan<Sample?>)[first, minimum, maximum, invalid, last])
            {
                if (sample == null || sample.ReceivedTime < start || sample.ReceivedTime > end)
                    continue;
                var seen = false;
                for (var i = bucketStart; i < output.Count && !seen; i++)
                    seen = output[i].Equals(sample);
                if (!seen)
                    output.Add(sample);
            }
        }
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
        while (recent.Count > 0 && (recent.Keys[0] + 1) * HistoryDefaults.RecentStepSeconds <= now - HistoryDefaults.DetailedSeconds)
        {
            var oldBucket = recent.Values[0];
            recent.RemoveAt(0);
            foreach (var point in oldBucket.Points())
            {
                var key = (long)Math.Floor(point.ReceivedTime / HistoryDefaults.OlderStepSeconds);
                if (!older.TryGetValue(key, out var bucket))
                    older[key] = bucket = new Bucket();
                bucket.Add(point);
            }
        }
        while (older.Count > 0 && (older.Keys[0] + 1) * HistoryDefaults.OlderStepSeconds < now - HistoryDefaults.RetentionSeconds)
            older.RemoveAt(0);
    }

    public Sample[] Read(double seconds, double now)
    {
        Trim(now);
        var start = now - Math.Min(seconds, HistoryDefaults.RetentionSeconds);
        // Buckets are ordered by key and a bucket's readings precede its end, so whole
        // buckets ending before the window are skipped without building their points.
        // The dashboard reads every series each second; this keeps that cost to the
        // shown window instead of the whole retained hour.
        var points = new List<Sample>();
        foreach (var (buckets, step) in new[] { (older, HistoryDefaults.OlderStepSeconds), (recent, HistoryDefaults.RecentStepSeconds) })
            for (var i = FirstEndingAfter(buckets, start, step); i < buckets.Count; i++)
                buckets.Values[i].AddPoints(points, start, now);
        return points.OrderBy(p => p.SourceTime).ToArray();
    }

    // Index of the first bucket whose end, (key + 1) * step, is after start.
    private static int FirstEndingAfter(SortedList<long, Bucket> buckets, double start, double step)
    {
        int low = 0, high = buckets.Count;
        while (low < high)
        {
            var middle = (low + high) / 2;
            if ((buckets.Keys[middle] + 1) * step <= start)
                low = middle + 1;
            else
                high = middle;
        }
        return low;
    }
}
