namespace CakeAdaptMonitor;

// History mode: capture files read once into compact buckets per series.
public static class HistoryModeDefaults
{
    public const int DetailedStepSeconds = 1, CoarseStepSeconds = 60;
    public static readonly int[] LevelSteps = [10, 60, 600];
    // One capture file (one router, one day) per history session (user, 2026-11-16).
    public const int MaximumFiles = 1;
    public const double MinimumRangeSeconds = 60, InitialRangeSeconds = 3600;
    // Router timestamps further than this from the recorder's receive time are not trusted
    // (a router can run hours off before NTP syncs); minutes cover publish delays.
    public const double ClockToleranceSeconds = 300;
    // Bucket keys start this far before the first reading, so earlier readings keep positive keys.
    public const double KeyMarginSeconds = 86400;
    public const string TooManyFiles = "Open one capture file at a time.";
    public const int InitialCapacity = 64;
}
