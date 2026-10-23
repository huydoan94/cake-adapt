using System.Reflection;
using System.Text;
using System.Text.Json;
namespace CakeAdaptMonitor;

public sealed class MetricStore
{
    private sealed class Series
    {
        public string Latest = "";
        public double Received;
        public long Order;
        public readonly MetricHistory History = new();
        public bool HasNumeric;
    }
    private readonly object gate = new();
    private readonly RatePeaks ratePeaks = new();
    private readonly Dictionary<string, string[]> defaults;
    private readonly Dictionary<(string Topic, string Record), string[]> headers = new();
    private readonly Queue<(string Topic, string Record)> headerOrder = new();
    private readonly Dictionary<MetricKey, Series> series = new();
    private readonly Queue<RecentEvent> events = new();
    private long order;
    private int headerBytes;
    public long SchemaErrors
    {
        get; private set;
    }
    public long EvictedSeries
    {
        get; private set;
    }
    public long EvictedHeaders
    {
        get; private set;
    }
    public int HeaderCount
    {
        get
        {
            lock (gate)
                return headers.Count;
        }
    }
    public int SeriesCount
    {
        get
        {
            lock (gate)
                return series.Count;
        }
    }
    public int MaxSeries { get; init; } = TelemetryDefaults.MaximumSeries;

    public MetricStore()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(TelemetryDefaults.SchemaResource)!;
        defaults = JsonSerializer.Deserialize<Dictionary<string, string[]>>(stream)!;
    }

    public void Accept(string topic, string payload, double received)
    {
        lock (gate)
        {
            events.Enqueue(new RecentEvent(topic, DateTimeOffset.FromUnixTimeSeconds((long)received).ToLocalTime().ToString(TelemetryDefaults.ClockFormat) +
                           "  " + topic + "  " + payload[..Math.Min(TelemetryDefaults.EventCharacters, payload.Length)]));
            while (events.Count > TelemetryDefaults.MaximumEvents)
                events.Dequeue();
            foreach (var line in payload.Split(ProtocolNames.LineSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                var cells = line.Split(ProtocolNames.CellSeparator).Select(s => s.Trim()).ToArray();
                var kind = cells[0];
                if (kind.EndsWith(ProtocolNames.HeaderSuffix))
                {
                    if (cells.Length < ProtocolNames.HeaderMinimumCells)
                        continue;
                    var key = (topic, kind[..^ProtocolNames.HeaderSuffix.Length]);
                    // A header replacement starts a new cache entry; bounded FIFO is enough here.
                    if (headers.ContainsKey(key))
                        RemoveHeader(key);
                    var names = cells[1..];
                    var bytes = Encoding.UTF8.GetByteCount(topic + kind + string.Join(ProtocolNames.CellSeparator, names));
                    while (headers.Count >= TelemetryDefaults.MaximumHeaders || (headers.Count > 0 && headerBytes + bytes > TelemetryDefaults.MaximumHeaderBytes))
                    {
                        var old = headerOrder.Dequeue();
                        if (headers.ContainsKey(old))
                        {
                            RemoveHeader(old);
                            EvictedHeaders++;
                        }
                    }
                    if (bytes > TelemetryDefaults.MaximumHeaderBytes)
                    {
                        EvictedHeaders++;
                        continue;
                    }
                    headers[key] = names;
                    headerBytes += bytes;
                    headerOrder.Enqueue(key);
                    // Repeated replacement headers must not leave an unbounded queue of stale keys.
                    if (headerOrder.Count > TelemetryDefaults.HeaderCompactionCount)
                    {
                        headerOrder.Clear();
                        foreach (var item in headers.Keys)
                            headerOrder.Enqueue(item);
                    }
                    continue;
                }
                if (!headers.TryGetValue((topic, kind), out var columns))
                    defaults.TryGetValue(kind, out columns);
                if (kind == ProtocolNames.CpuRecord && (columns == null || columns.Length != cells.Length - 1))
                {
                    var count = cells.Length - ProtocolNames.CpuFixedCells;
                    if (count <= 0)
                        continue;
                    columns = [ProtocolNames.LogDateTime, ProtocolNames.LogTimestamp, ProtocolNames.ReadTime, ProtocolNames.CpuUsage,
                        .. Enumerable.Range(0, count - 1).Select(i => string.Format(ProtocolNames.CpuUsageFormat, i))];
                }
                if (columns == null)
                    continue;
                if (columns.Length != cells.Length - 1)
                {
                    SchemaErrors++;
                    continue;
                }
                var fields = new Dictionary<string, string>();
                for (var i = 0; i < columns.Length; i++)
                    fields[columns[i]] = cells[i + 1];
                var stamp = Telemetry.Number(fields.GetValueOrDefault(ProtocolNames.LogTimestamp, ""));
                if (!stamp.HasValue || stamp < 0 || stamp > TelemetryDefaults.MaximumUnixSeconds)
                    stamp = received;
                var dimension = string.Join(ProtocolNames.DimensionSeparator, new[] { ProtocolNames.Reflector, ProtocolNames.CpuId }.Where(fields.ContainsKey).Select(k => fields[k]));
                foreach (var (name, text) in fields)
                {
                    if (name is ProtocolNames.LogDateTime or ProtocolNames.LogTimestamp or ProtocolNames.Reflector or ProtocolNames.CpuId)
                        continue;
                    var key = new MetricKey(topic, kind, dimension, name);
                    if (!series.TryGetValue(key, out var data))
                    {
                        if (series.Count >= MaxSeries)
                        {
                            series.Remove(series.MinBy(p => p.Value.Order).Key);
                            EvictedSeries++;
                        }
                        data = new Series();
                        series[key] = data;
                    }
                    data.Latest = text;
                    data.Received = received;
                    data.Order = ++order;
                    var number = Telemetry.Number(text);
                    var invalidQueue = name.EndsWith(ProtocolNames.QueueSuffix) &&
                        fields.GetValueOrDefault(name.Replace(ProtocolNames.QueueSuffix, ProtocolNames.ValidSuffix)) == ProtocolNames.InvalidQueue;
                    if (invalidQueue)
                    {
                        number = null;
                        data.Latest = TelemetryDefaults.Unavailable;
                    }
                    if (number.HasValue || invalidQueue || data.HasNumeric)
                    {
                        data.HasNumeric |= number.HasValue;
                        var sample = new Sample(stamp.Value, received, number);
                        data.History.Add(sample);
                        ratePeaks.Record(topic, name, sample);
                    }
                }
            }
        }
    }

    private void RemoveHeader((string Topic, string Record) key)
    {
        var names = headers[key];
        headerBytes -= Encoding.UTF8.GetByteCount(key.Topic + key.Record + ProtocolNames.HeaderSuffix + string.Join(ProtocolNames.CellSeparator, names));
        headers.Remove(key);
    }

    public MetricView[] Snapshot(double seconds, double? at = null)
    {
        var now = at ?? Telemetry.Now;
        lock (gate)
            return series.Select(p => new MetricView(p.Key, p.Value.Latest, p.Value.Received,
                p.Value.History.Read(seconds, now)) { RatePeak = ratePeaks.Read(p.Key.Topic, p.Key.Name) }).ToArray();
    }
    // Advance only on the screen timer, not on source/search/settings refreshes.
    public void PublishRatePeaks()
    {
        lock (gate) ratePeaks.Publish();
    }
    public RecentEvent[] Events()
    {
        lock (gate)
            return events.ToArray();
    }
}
