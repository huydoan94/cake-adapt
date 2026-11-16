namespace CakeAdaptMonitor;

// Recorder capture lines (mosquitto_sub -F %j): one JSON object per line.
public static class CaptureFormat
{
    public const string TimeFormat = "yyyy-MM-dd'T'HH:mm:ss.FFFFFFzzz", GzipExtension = ".gz";
    public const string TimeField = "tst", TopicField = "topic", PayloadField = "payload";
    // The loader checks for cancellation and reports progress every this many lines,
    // at most once per ProgressInterval.
    public const int CancelCheckLines = 4096;
    public static readonly TimeSpan ProgressInterval = TimeSpan.FromMilliseconds(100);
}
