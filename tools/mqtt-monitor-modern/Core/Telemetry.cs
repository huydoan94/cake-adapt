using System.Globalization;
using System.Text;

namespace CakeAdaptMonitor;

public static class Telemetry
{
    public static string Age(double received) => Math.Max(0, Now - received).ToString(TelemetryDefaults.AgeFormat) + TelemetryDefaults.SecondsUnit;
    public static double Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / TelemetryDefaults.MillisecondsPerSecond;
    public static double? Number(string value) =>
        double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var n) &&
        double.IsFinite(n) ? n : null;
    public static string Format(double value) => Math.Abs(value) >= TelemetryDefaults.ScientificLarge || (value != 0 && Math.Abs(value) < TelemetryDefaults.ScientificSmall)
        ? value.ToString(TelemetryDefaults.ScientificFormat, CultureInfo.InvariantCulture) : value.ToString(TelemetryDefaults.NumericFormat, CultureInfo.InvariantCulture).TrimEnd('0').TrimEnd('.');
    public static double Mean(double[] values)
    {
        var scale = values.Max(Math.Abs);
        return scale == 0 ? 0 : (values.Sum(v => v / scale) / values.Length) * scale;
    }
    public static string Unit(string name) => name.EndsWith(ProtocolNames.KilobitsSuffix) ? ProtocolNames.KilobitsUnit :
        name.EndsWith(ProtocolNames.MicrosecondsSuffix) || name.EndsWith(ProtocolNames.BaselineSuffix) || name.EndsWith(ProtocolNames.DelayThresholdSuffix) ? ProtocolNames.MicrosecondsUnit :
        name.EndsWith(ProtocolNames.KibibytesSuffix) ? ProtocolNames.KibibytesUnit : name.EndsWith(ProtocolNames.PercentSuffix) || name.EndsWith(ProtocolNames.UsageSuffix) ? ProtocolNames.PercentUnit : "";
    public static IEnumerable<MetricView> SelectSeries(IEnumerable<MetricView> metrics) =>
        metrics.GroupBy(m => (m.Topic, m.Dimension, m.Name)).Select(g => g
            .OrderByDescending(m => m.Samples.Length > 0)
            .ThenByDescending(m => m.ReceivedTime)
            .ThenByDescending(m => m.Record == ProtocolNames.SummaryRecord ? ProtocolNames.SummaryPreference : m.Record == ProtocolNames.LoadRecord ? ProtocolNames.LoadPreference : ProtocolNames.OtherPreference).First());
    public static void ValidateTopic(string topic)
    {
        if (string.IsNullOrWhiteSpace(topic) || topic.Contains(ProtocolNames.InvalidCharacter) || Encoding.UTF8.GetByteCount(topic) > ConnectionDefaults.MaximumTopicBytes)
            throw new ArgumentException("Enter a valid subscription topic.");
        var parts = topic.Split(ProtocolNames.TopicSeparator);
        for (var i = 0; i < parts.Length; i++)
            if ((parts[i].Contains(ProtocolNames.SingleWildcard) && parts[i] != ProtocolNames.SingleWildcard) ||
                (parts[i].Contains(ProtocolNames.MultiWildcard) && (parts[i] != ProtocolNames.MultiWildcard || i != parts.Length - 1)))
                throw new ArgumentException("Use + as a whole level and # as the final whole level.");
    }
}
