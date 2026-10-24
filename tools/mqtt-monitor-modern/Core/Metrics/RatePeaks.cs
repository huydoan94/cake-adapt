namespace CakeAdaptMonitor;

// Owned by MetricStore and accessed only under its existing lock. Global
// traffic fields repeat in LOAD/SUMMARY/per-reflector DATA: share their peak
// by topic/direction so a later low-valued record cannot hide an earlier peak.
public sealed class RatePeaks
{
    private sealed class Interval
    {
        public Sample? Pending;
        public Sample? Displayed;
        public double Latest;
    }
    private readonly Dictionary<(string Topic, string Name), Interval> intervals = new();
    private readonly int capacity;
    public RatePeaks(int capacity = TelemetryDefaults.MaximumSeries) => this.capacity = capacity;
    public void Record(string topic, string name, Sample sample)
    {
        if (name is not (MetricNames.DownloadTraffic or MetricNames.UploadTraffic) || !sample.Value.HasValue)
            return;
        var key = (topic, name);
        if (!intervals.TryGetValue(key, out var interval))
        {
            if (intervals.Count >= capacity)
                intervals.Remove(intervals.MinBy(p => p.Value.Latest).Key);
            intervals[key] = interval = new Interval();
        }
        interval.Latest = sample.ReceivedTime;
        if (interval.Pending == null || sample.Value >= interval.Pending.Value)
            interval.Pending = sample;
    }
    public void Publish()
    {
        foreach (var interval in intervals.Values)
        {
            if (interval.Pending == null)
                continue;
            interval.Displayed = interval.Pending;
            interval.Pending = null;
        }
    }
    public Sample? Read(string topic, string name) => intervals.TryGetValue((topic, name), out var interval)
        ? interval.Displayed ?? interval.Pending : null;
}
