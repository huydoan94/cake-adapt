using System.Globalization;

namespace CakeAdaptMonitor;

// Numeric parsing, raw units and measurement formatting shared by core and UI.
public static class Telemetry
{
    public static double Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() /
        TelemetryDefaults.MillisecondsPerSecond;

    public static double? Number(string text) =>
        double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) &&
        double.IsFinite(number) ? number : null;

    public static string Format(double value)
    {
        var magnitude = Math.Abs(value);
        if (magnitude >= TelemetryDefaults.ScientificLarge ||
            (value != 0 && magnitude < TelemetryDefaults.ScientificSmall))
            return value.ToString(TelemetryDefaults.ScientificFormat, CultureInfo.InvariantCulture);
        return value.ToString(TelemetryDefaults.NumericFormat, CultureInfo.InvariantCulture)
            .TrimEnd('0').TrimEnd('.');
    }

    public static double Mean(double[] values)
    {
        var scale = values.Max(Math.Abs);
        return scale == 0 ? 0 : (values.Sum(value => value / scale) / values.Length) * scale;
    }

    public static string Age(double received) =>
        Math.Max(0, Now - received).ToString(TelemetryDefaults.AgeFormat) + TelemetryDefaults.SecondsUnit;

    public static string Unit(string name)
    {
        if (name.EndsWith(ProtocolNames.KilobitsSuffix))
            return ProtocolNames.KilobitsUnit;
        if (name.EndsWith(ProtocolNames.MicrosecondsSuffix) ||
            name.EndsWith(ProtocolNames.BaselineSuffix) || name.EndsWith(ProtocolNames.DelayThresholdSuffix))
            return ProtocolNames.MicrosecondsUnit;
        if (name.EndsWith(ProtocolNames.KibibytesSuffix))
            return ProtocolNames.KibibytesUnit;
        if (name.EndsWith(ProtocolNames.PercentSuffix) || name.EndsWith(ProtocolNames.UsageSuffix))
            return ProtocolNames.PercentUnit;
        return "";
    }
}
