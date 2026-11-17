namespace CakeAdaptMonitor;

// A non-measurement record (INFO, SHAPER, WARNING, SYSLOG, ...) kept for the Events page.
public sealed record HistoryEvent(double Time, string Topic, string Text);
