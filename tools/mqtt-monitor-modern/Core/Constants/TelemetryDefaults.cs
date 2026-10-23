namespace CakeAdaptMonitor;

public static class TelemetryDefaults
{
    public const int MaximumSeries = 2048;
    public const int MaximumHeaders = 256;
    public const int MaximumHeaderBytes = 1024 * 1024;
    public const int HeaderCompactionCount = 512;
    public const int MaximumEvents = 150;
    public const int EventCharacters = 2000;
    public const long MaximumUnixSeconds = 32503680000;
    public const double MillisecondsPerSecond = 1000;
    public const double KilobitsPerMegabit = 1000;
    public const double KibibytesPerMebibyte = 1024;
    public const double ScientificLarge = 1e7;
    public const double ScientificSmall = .001;
    public const string ScientificFormat = "G4";
    public const string NumericFormat = "N2";
    public const string AgeFormat = "F1";
    public const string ClockFormat = "HH:mm:ss";
    public const string SchemaResource = "CakeAdaptMonitor.schemas.json";
    public const string Unavailable = "Unavailable";
    public const string EmptyValue = "—";
    public const string SecondsUnit = " s";
}
public static class HistoryDefaults
{
    public const double RetentionSeconds = 3600;
    public const double DetailedSeconds = 600;
    public const int RecentStepSeconds = 2;
    public const int OlderStepSeconds = 10;
    public const double ClockResetSeconds = 5;
}
