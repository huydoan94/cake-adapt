using Avalonia.Media;

namespace CakeAdaptMonitor;

public sealed record PlotLine(string Label, Sample[] Points, Color Color);

// Convert raw measurements to display units and labels. No selection or drawing state.
internal static class PlotSeries
{
    public static string Unit(string report, MetricView[] metrics) => report switch
    {
        ReportNames.Throughput => ReportUnits.Rate,
        ReportNames.Latency or ReportNames.TcpQueues => ReportUnits.Delay,
        ReportNames.Memory => ReportUnits.Memory,
        ReportNames.Cpu => ReportUnits.Cpu,
        _ => metrics.FirstOrDefault()?.Unit ?? ReportUnits.Count
    };

    public static PlotLine[] Create(string report, MetricView[] metrics)
    {
        var divisor = report switch
        {
            ReportNames.Throughput => ReportUnits.RateDivisor,
            ReportNames.Latency or ReportNames.TcpQueues => ReportUnits.DelayDivisor,
            ReportNames.Memory => ReportUnits.MemoryDivisor,
            _ => 1
        };
        var multipleSources = metrics.Select(metric => metric.Topic).Distinct().Count() > 1;
        return metrics.Select((metric, i) => new PlotLine(Label(metric, multipleSources),
            metric.Samples.Select(sample => sample with { Value = sample.Value / divisor }).ToArray(),
            UiTheme.Palette[i % UiTheme.Palette.Length])).ToArray();
    }

    private static string Label(MetricView metric, bool multipleSources)
    {
        var label = metric.Name.Replace(UiText.TrafficFieldSuffix, UiText.TrafficLabel)
            .Replace(UiText.RateFieldSuffix, UiText.RateLabel)
            .Replace(UiText.DelayFieldSuffix, UiText.DelayLabel).Replace(UiText.MemoryFieldSuffix, "");
        if (multipleSources)
            label += UiText.SourceSeparator + DisplayText.ShortTopic(metric.Topic);
        if (metric.Dimension.Length > 0)
            label += UiText.SourceSeparator + metric.Dimension;
        return label;
    }
}
