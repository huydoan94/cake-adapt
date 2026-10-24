namespace CakeAdaptMonitor;

public sealed record MetricKey(string Topic, string Record, string Dimension, string Name);
