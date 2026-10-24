namespace CakeAdaptMonitor;

public sealed record TraceKey(string Topic, string Dimension, string Name)
{
    public static TraceKey From(MetricView metric) => new(metric.Topic, metric.Dimension, metric.Name);
}
public sealed record ChartSelection(bool Automatic, HashSet<TraceKey> Keys);
