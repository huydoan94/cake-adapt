using CakeAdaptMonitor;

static partial class Tests
{
    private static void TimeTickChecks()
    {
        Check(TimeTicks.Step(300) == 60 && TimeTicks.Step(6 * 3600) == 3600 && TimeTicks.Step(3 * 86400) == 43200 && TimeTicks.Step(30) == 5,
            "tick steps follow the range: minutes for 5 min, hours for 6 h");
        var local = new DateTimeOffset(2026, 11, 15, 12, 0, 30, TimeZoneInfo.Local.GetUtcOffset(new DateTime(2026, 11, 15, 12, 0, 30)));
        var start = local.ToUnixTimeSeconds();
        var ticks = TimeTicks.Times(start, start + 300);
        Check(ticks.Length is >= 4 and <= 8 && ticks.All(t => DateTimeOffset.FromUnixTimeSeconds((long)t).ToLocalTime().Second == 0) &&
              ticks.All(t => t >= start && t <= start + 300),
            "ticks fall on whole local minutes inside the range");
        var lateEvening = new DateTimeOffset(2026, 11, 15, 23, 0, 0, TimeZoneInfo.Local.GetUtcOffset(new DateTime(2026, 11, 15, 23, 0, 0))).ToUnixTimeSeconds();
        Check(TimeTicks.CrossesMidnight(lateEvening, lateEvening + 7200) && !TimeTicks.CrossesMidnight(start, start + 300) &&
              TimeTicks.Label(lateEvening + 3600, 3600, true).Contains("11-16"),
            "labels show the date when the range crosses midnight");
        Check(TimeTicks.Label(start + 30, 60, false) == "12:01" && TimeTicks.Label(start + 35, 5, false) == "12:01:05",
            "labels show seconds only for sub-minute steps");
        Check(!new TrendChart().RangeTicks, "live charts keep their own time axis");
    }
}
