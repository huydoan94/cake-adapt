using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Text.Json;

namespace CakeAdaptMonitor;

public sealed record LoadProgress(int File, int Files, long Records, long BadLines, double Fraction);

// Reads recorder captures (one JSON object per line: tst, topic, payload), plain or
// gzip, into a sealed HistoryStore. Files are read in name order, one after another,
// because a source's *_HEADER definitions can carry over into the next day's file.
public static class CaptureLoader
{
    public static Task<HistoryStore> LoadAsync(IReadOnlyList<string> paths, IProgress<LoadProgress>? progress, CancellationToken token)
    {
        var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        var files = paths.Distinct(comparer).OrderBy(Path.GetFileName, comparer).ToArray();
        if (files.Length > HistoryModeDefaults.MaximumFiles)
            throw new ArgumentException(HistoryModeDefaults.TooManyFiles);
        token.ThrowIfCancellationRequested();
        return Task.Run(() => Load(files, progress, token), token);
    }

    private static HistoryStore Load(string[] files, IProgress<LoadProgress>? progress, CancellationToken token)
    {
        var store = new HistoryStore();
        var clock = Stopwatch.StartNew();
        for (var index = 0; index < files.Length; index++)
        {
            using var file = File.OpenRead(files[index]);
            using Stream content = files[index].EndsWith(CaptureFormat.GzipExtension, StringComparison.OrdinalIgnoreCase)
                ? new GZipStream(file, CompressionMode.Decompress) : file;
            using var reader = new StreamReader(content);
            var lines = 0L;
            while (reader.ReadLine() is { } line)
            {
                if (++lines % CaptureFormat.CancelCheckLines == 0)
                {
                    token.ThrowIfCancellationRequested();
                    if (progress != null && clock.Elapsed >= CaptureFormat.ProgressInterval)
                    {
                        clock.Restart();
                        progress.Report(new LoadProgress(index + 1, files.Length, store.Records, store.BadLines,
                            (index + (double)file.Position / Math.Max(1, file.Length)) / files.Length));
                    }
                }
                if (line.Length == 0)
                    continue;
                if (!TryParse(line, out var time, out var topic, out var payload))
                {
                    store.CountBad();
                    continue;
                }
                store.Accept(topic, payload, time);
            }
        }
        store.Seal();
        progress?.Report(new LoadProgress(files.Length, files.Length, store.Records, store.BadLines, 1));
        return store;
    }

    private static bool TryParse(string line, out double time, out string topic, out string payload)
    {
        time = 0;
        topic = payload = "";
        try
        {
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            // Valid JSON that is not a capture record (42, null, [], wrong field types) is a bad line.
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty(CaptureFormat.TimeField, out var tst) || tst.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty(CaptureFormat.TopicField, out var topicValue) || topicValue.ValueKind != JsonValueKind.String ||
                !root.TryGetProperty(CaptureFormat.PayloadField, out var payloadValue) || payloadValue.ValueKind != JsonValueKind.String ||
                !DateTimeOffset.TryParseExact(tst.GetString(), CaptureFormat.TimeFormat, CultureInfo.InvariantCulture, DateTimeStyles.None, out var at))
                return false;
            time = at.ToUnixTimeMilliseconds() / TelemetryDefaults.MillisecondsPerSecond;
            topic = topicValue.GetString() ?? "";
            payload = payloadValue.GetString() ?? "";
            return topic.Length > 0;
        }
        catch (JsonException)
        {
            return false;
        }
    }
}
