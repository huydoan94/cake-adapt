namespace CakeAdaptMonitor;

public sealed record MetricView(MetricKey Key, string Latest, double ReceivedTime, Sample[] Samples)
{
    public Sample? RatePeak { get; init; }
    public string Topic => Key.Topic;
    public string Record => Key.Record;
    public string Dimension => Key.Dimension;
    public string Name => Key.Name;
    public string Unit => Telemetry.Unit(Name);
    public string Age => Telemetry.Age(ReceivedTime);
    public double[] Values => Samples.Where(p => p.Value.HasValue).Select(p => p.Value!.Value).ToArray();
    public string Min => Values.Length == 0 ? TelemetryDefaults.EmptyValue : Telemetry.Format(Values.Min());
    public string Mean => Values.Length == 0 ? TelemetryDefaults.EmptyValue : Telemetry.Format(Telemetry.Mean(Values));
    public string Max => Values.Length == 0 ? TelemetryDefaults.EmptyValue : Telemetry.Format(Values.Max());
    public int Count => Values.Length;
}
