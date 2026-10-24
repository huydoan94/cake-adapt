namespace CakeAdaptMonitor;

internal readonly record struct DecodedMetric(MetricKey Key, string Text, double SourceTime, double? Number, bool InvalidQueue);

// Decode protocol records only. History, display batches and eviction belong to the store.
internal sealed class RecordDecoder
{
    public RecordSchemas Schemas { get; } = new();
    public long Errors
    {
        get; private set;
    }

    public IEnumerable<DecodedMetric> Decode(string topic, string payload, double received)
    {
        foreach (var line in payload.Split(ProtocolNames.LineSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            var cells = line.Split(ProtocolNames.CellSeparator).Select(cell => cell.Trim()).ToArray();
            var kind = cells[0];
            if (kind.EndsWith(ProtocolNames.HeaderSuffix))
            {
                Schemas.Replace(topic, kind, cells);
                continue;
            }
            var columns = Schemas.Resolve(topic, kind, cells.Length);
            if (columns == null)
                continue;
            if (columns.Length != cells.Length - 1)
            {
                Errors++;
                continue;
            }
            var fields = new Dictionary<string, string>();
            for (var i = 0; i < columns.Length; i++)
                fields[columns[i]] = cells[i + 1];

            var stamp = Telemetry.Number(fields.GetValueOrDefault(ProtocolNames.LogTimestamp, ""));
            if (!stamp.HasValue || stamp < 0 || stamp > TelemetryDefaults.MaximumUnixSeconds)
                stamp = received;
            var dimension = string.Join(ProtocolNames.DimensionSeparator,
                new[] { ProtocolNames.Reflector, ProtocolNames.CpuId }.Where(fields.ContainsKey).Select(key => fields[key]));
            foreach (var (name, text) in fields)
            {
                if (name is ProtocolNames.LogDateTime or ProtocolNames.LogTimestamp or ProtocolNames.Reflector or ProtocolNames.CpuId)
                    continue;
                var invalidQueue = name.EndsWith(ProtocolNames.QueueSuffix) &&
                    fields.GetValueOrDefault(name.Replace(ProtocolNames.QueueSuffix, ProtocolNames.ValidSuffix)) == ProtocolNames.InvalidQueue;
                yield return new DecodedMetric(new MetricKey(topic, kind, dimension, name),
                    invalidQueue ? TelemetryDefaults.Unavailable : text, stamp.Value,
                    invalidQueue ? null : Telemetry.Number(text), invalidQueue);
            }
        }
    }
}
