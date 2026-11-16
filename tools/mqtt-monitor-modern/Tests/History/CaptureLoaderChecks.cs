using System.IO.Compression;
using System.Text;
using CakeAdaptMonitor;

static partial class Tests
{
    private static string CaptureLine(DateTimeOffset at, string topic, string payload) =>
        System.Text.Json.JsonSerializer.Serialize(new { tst = at.ToString("yyyy-MM-dd'T'HH:mm:ss.ffffff", System.Globalization.CultureInfo.InvariantCulture) + at.ToString("zzz").Replace(":", ""), topic, qos = 0, retain = 0, payloadlen = payload.Length, payload });

    // Writes one day of LOAD lines (one per second for `seconds`) and returns the text.
    private static string CaptureDay(DateTimeOffset day, int seconds)
    {
        var text = new StringBuilder();
        for (var i = 0; i < seconds; i++)
        {
            var at = day.AddSeconds(i);
            text.Append(CaptureLine(at, "cake-adapt/a", $"LOAD; date; {at.ToUnixTimeMilliseconds() / 1000.0:R}; 100; {1000 + i}; 500; 100000; 20000")).Append('\n');
        }
        return text.ToString();
    }

    private static async Task CaptureLoaderChecks()
    {
        var folder = Directory.CreateTempSubdirectory("cake-history-");
        try
        {
            var day1 = new DateTimeOffset(2026, 11, 14, 0, 0, 0, TimeSpan.FromHours(-6));
            var day2 = day1.AddDays(1);
            var plain = Path.Combine(folder.FullName, "mqtt-2026-11-14.jsonl");
            File.WriteAllText(plain, CaptureDay(day1, 300));
            var gzip = Path.Combine(folder.FullName, "mqtt-2026-11-15.jsonl.gz");
            using (var file = File.Create(gzip))
            using (var zip = new GZipStream(file, CompressionLevel.Fastest))
                zip.Write(Encoding.UTF8.GetBytes(CaptureDay(day2, 300)));

            var reports = new List<LoadProgress>();
            var both = await CaptureLoader.LoadAsync([gzip, plain, plain], new Progress<LoadProgress>(reports.Add), CancellationToken.None);
            Check(both.Records == 600 && both.BadLines == 0, "the loader reads plain and gzip captures, each file once");
            Check(both.Start == day1.ToUnixTimeSeconds() && both.End == day2.ToUnixTimeSeconds() + 299,
                "the loaded history spans from the first to the last record");

            var cut = Path.Combine(folder.FullName, "mqtt-2026-11-16.jsonl");
            var day3 = CaptureDay(day2.AddDays(1), 10);
            File.WriteAllText(cut, day3[..(day3.Length - 25)]);
            var truncated = await CaptureLoader.LoadAsync([cut], null, CancellationToken.None);
            Check(truncated.Records == 9 && truncated.BadLines == 1, "a cut-off last line is counted as bad and the rest is kept");

            var tooMany = Enumerable.Range(1, 8).Select(i => Path.Combine(folder.FullName, $"day{i}.jsonl")).ToArray();
            var refused = false;
            try { await CaptureLoader.LoadAsync(tooMany, null, CancellationToken.None); }
            catch (ArgumentException e) { refused = e.Message == HistoryModeDefaults.TooManyFiles; }
            Check(refused, "more than seven capture files are refused");

            var cancelled = false;
            try { await CaptureLoader.LoadAsync([plain], null, new CancellationToken(true)); }
            catch (OperationCanceledException) { cancelled = true; }
            Check(cancelled, "a cancelled load stops");
        }
        finally { folder.Delete(true); }
    }
}
