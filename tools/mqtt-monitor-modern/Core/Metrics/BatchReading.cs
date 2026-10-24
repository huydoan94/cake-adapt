namespace CakeAdaptMonitor;

// Preserve original field text and receipt time. Non-numeric fields keep the
// last reading, including an invalid-only batch.
public sealed record BatchReading(string Text, double ReceivedTime, double? Number);
