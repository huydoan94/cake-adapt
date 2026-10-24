using System.Reflection;
using System.Text;
using System.Text.Json;

namespace CakeAdaptMonitor;

// Accessed only while MetricStore holds its lock. Replacement refreshes FIFO order.
internal sealed class RecordSchemas
{
    private sealed record Header(string[] Columns, int Bytes, LinkedListNode<(string Topic, string Record)> Node);
    private readonly Dictionary<string, string[]> defaults;
    private readonly Dictionary<(string Topic, string Record), Header> headers = new();
    private readonly LinkedList<(string Topic, string Record)> order = new();
    private int bytes;

    public int Count => headers.Count;
    public long Evicted
    {
        get; private set;
    }

    public RecordSchemas()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(TelemetryDefaults.SchemaResource)!;
        defaults = JsonSerializer.Deserialize<Dictionary<string, string[]>>(stream)!;
    }

    public void Replace(string topic, string kind, string[] cells)
    {
        if (cells.Length < ProtocolNames.HeaderMinimumCells)
            return;

        var key = (topic, kind[..^ProtocolNames.HeaderSuffix.Length]);
        Remove(key);
        var columns = cells[1..];
        var size = Encoding.UTF8.GetByteCount(topic + kind + string.Join(ProtocolNames.CellSeparator, columns));
        while (headers.Count > 0 && (headers.Count >= TelemetryDefaults.MaximumHeaders || bytes + size > TelemetryDefaults.MaximumHeaderBytes))
        {
            Remove(order.First!.Value);
            Evicted++;
        }
        if (size > TelemetryDefaults.MaximumHeaderBytes)
        {
            Evicted++;
            return;
        }
        headers.Add(key, new Header(columns, size, order.AddLast(key)));
        bytes += size;
    }

    private void Remove((string Topic, string Record) key)
    {
        if (!headers.Remove(key, out var header))
            return;
        bytes -= header.Bytes;
        order.Remove(header.Node);
    }

    public string[]? Resolve(string topic, string kind, int cellCount)
    {
        var columns = headers.TryGetValue((topic, kind), out var header)
            ? header.Columns : defaults.GetValueOrDefault(kind);
        if (kind != ProtocolNames.CpuRecord || columns?.Length == cellCount - 1)
            return columns;

        var count = cellCount - ProtocolNames.CpuFixedCells;
        return count <= 0 ? null : [
            ProtocolNames.LogDateTime, ProtocolNames.LogTimestamp, ProtocolNames.ReadTime,
            ProtocolNames.CpuUsage,
            .. Enumerable.Range(0, count - 1).Select(i => string.Format(ProtocolNames.CpuUsageFormat, i))
        ];
    }
}
