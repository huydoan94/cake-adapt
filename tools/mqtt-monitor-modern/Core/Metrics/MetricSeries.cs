namespace CakeAdaptMonitor;

// Mutable state for one full MetricKey. Owned exclusively by MetricStore's lock.
internal sealed class MetricSeries
{
    private readonly MetricHistory history = new();
    private string latest = "";
    private double received;
    private bool hasNumeric;
    private BatchReading? pendingBatch;
    private BatchReading? displayedBatch;
    public long Order
    {
        get; private set;
    }

    public Sample? Add(DecodedMetric metric, double receivedTime, long order)
    {
        latest = metric.Text;
        received = receivedTime;
        Order = order;
        if (pendingBatch == null || !pendingBatch.Number.HasValue || metric.Number >= pendingBatch.Number)
            pendingBatch = new BatchReading(latest, received, metric.Number);

        if (!metric.Number.HasValue && !metric.InvalidQueue && !hasNumeric)
            return null;
        hasNumeric |= metric.Number.HasValue;
        var sample = new Sample(metric.SourceTime, received, metric.Number);
        history.Add(sample);
        return sample;
    }

    public void PublishBatch()
    {
        if (pendingBatch == null)
            return;
        displayedBatch = pendingBatch;
        pendingBatch = null;
    }

    public MetricView Snapshot(MetricKey key, double seconds, double now, Sample? ratePeak) =>
        new(key, latest, received, history.Read(seconds, now))
        {
            RatePeak = ratePeak,
            BatchPeak = displayedBatch ?? pendingBatch
        };
}
