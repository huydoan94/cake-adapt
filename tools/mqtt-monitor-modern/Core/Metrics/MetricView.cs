namespace CakeAdaptMonitor;

public sealed record MetricView(MetricKey Key, string Latest, double ReceivedTime, Sample[] Samples)
{
    public Sample? RatePeak
    {
        get; init;
    }
    // Exact statistics over a history range; when set, the table uses them instead of the samples.
    public RangeStats? Stats
    {
        get; init;
    }
    public BatchReading? BatchPeak
    {
        get; init;
    }
    public string Topic => Key.Topic;
    public string Record => Key.Record;
    public string Dimension => Key.Dimension;
    public string Name => Key.Name;
    public string Unit => Telemetry.Unit(Name);
    public string Age => Telemetry.Age(ReceivedTime);
    public double[] Values => Samples.Where(p => p.Value.HasValue).Select(p => p.Value!.Value).ToArray();
    public string Min => Stats != null ? Telemetry.Format(Stats.Min) : Values.Length == 0 ? TelemetryDefaults.EmptyValue : Telemetry.Format(Values.Min());
    public string Mean => Stats != null ? Telemetry.Format(Stats.Mean) : Values.Length == 0 ? TelemetryDefaults.EmptyValue : Telemetry.Format(Telemetry.Mean(Values));
    public string Max => Stats != null ? Telemetry.Format(Stats.Max) : Values.Length == 0 ? TelemetryDefaults.EmptyValue : Telemetry.Format(Values.Max());
    public int Count => Stats != null ? (int)Math.Min(int.MaxValue, Stats.Count) : Values.Length;
}
