namespace CakeAdaptMonitor;

public sealed record Sample(double SourceTime, double ReceivedTime, double? Value);
