namespace CakeAdaptMonitor;

public static class ProtocolNames
{
    public const char CellSeparator = ';', LineSeparator = '\n', TopicSeparator = '/', InvalidCharacter = '\0';
    public const string HeaderSuffix = "_HEADER", CpuRecord = "CPU", SummaryRecord = "SUMMARY", LoadRecord = "LOAD";
    public const string LogDateTime = "LOG_DATETIME", LogTimestamp = "LOG_TIMESTAMP", ReadTime = "STATS_READ_TIME";
    public const string CpuUsage = "CPU_USAGE", CpuUsageFormat = "CPU{0}_USAGE";
    public const string Reflector = "REFLECTOR", CpuId = "CPU_ID", DimensionSeparator = " / ";
    public const string QueueSuffix = "_QUEUE_US", ValidSuffix = "_QUEUE_VALID", InvalidQueue = "0";
    public const string KilobitsSuffix = "KBPS", MicrosecondsSuffix = "_US", BaselineSuffix = "BASELINE", DelayThresholdSuffix = "DELAY_THR";
    public const string KibibytesSuffix = "_KB", PercentSuffix = "PERCENT", UsageSuffix = "USAGE";
    public const string KilobitsUnit = "kbit/s", MicrosecondsUnit = "µs", KibibytesUnit = "KiB", PercentUnit = "%";
    // Load conditions ending in BloatSuffix mean delay above the router's threshold.
    public const string DataRecord = "DATA", DownloadCondition = "DL_LOAD_CONDITION", UploadCondition = "UL_LOAD_CONDITION";
    public const string BloatSuffix = "_bb", Download = "DL", Upload = "UL";
    public const string SingleWildcard = "+", MultiWildcard = "#", UriSchemeMarker = "://";
    public const int HeaderMinimumCells = 3, CpuFixedCells = 4;
    public const int SummaryPreference = 3, LoadPreference = 2, OtherPreference = 1;
}
