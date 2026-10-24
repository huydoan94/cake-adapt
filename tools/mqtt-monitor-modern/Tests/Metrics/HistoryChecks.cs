using CakeAdaptMonitor;

static partial class Tests
{
    private static void HistoryChecks()
    {
        var hourly = new MetricStore();
        hourly.Accept("history", "X_HEADER; LOG_TIMESTAMP; VALUE", 0);
        for (var i = 0; i <= 18000; i++)
        {
            var time = i / 5.0;
            hourly.Accept("history", $"X; {time:R}; {(i == 7 ? 9999 : i % 10)}", time);
        }
        var hourPoints = hourly.Snapshot(3600, 3600).Single().Samples;
        Check(hourPoints.Min(p => p.ReceivedTime) <= 1 && hourPoints.Max(p => p.ReceivedTime) == 3600, "five-Hz input retains the full hour");
        Check(hourPoints.Any(p => p.Value == 9999), "downsampling preserves an old isolated spike");
        var fiveMinutes = hourly.Snapshot(300, 3600).Single().Samples;
        Check(fiveMinutes.Min(p => p.ReceivedTime) <= 3302 && fiveMinutes.Max(p => p.ReceivedTime) == 3600, "five-minute window no longer cuts off at 600 readings");
        var history = new MetricHistory();
        for (var i = 0; i <= 72000; i++)
            history.Add(new Sample(i / 20.0, i / 20.0, i % 50));
        Check(history.BucketCount <= 610 && history.Read(3600, 3600).Length <= 3050, "high-rate history has bounded buckets and render points");
        Check(history.Read(3600, 8000).Length == 0, "expired history is removed by time");
    }

}
