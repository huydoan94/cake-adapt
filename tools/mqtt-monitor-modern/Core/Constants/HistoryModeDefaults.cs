namespace CakeAdaptMonitor;

// History mode: capture files read once into compact buckets per series.
public static class HistoryModeDefaults
{
    public const int DetailedStepSeconds = 1, CoarseStepSeconds = 60;
    public static readonly int[] LevelSteps = [10, 60, 600];
    public const int MaximumFiles = 7;
    public const double MinimumRangeSeconds = 60, InitialRangeSeconds = 3600;
    // Router timestamps further than this from the recorder's receive time are not trusted.
    public const double ClockToleranceSeconds = 86400;
    public const string TooManyFiles = "Open at most 7 capture files (days) at a time.";
    public const int InitialCapacity = 64;
}
