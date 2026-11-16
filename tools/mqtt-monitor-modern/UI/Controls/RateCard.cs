using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

// One rate field, shown for up to UiLayout.RateCardSources sources, one line each.
// Sources are listed in topic order so a line does not jump between routers.
internal sealed class RateCard : Border
{
    private readonly string field;
    private readonly StackPanel lines = new() { Spacing = UiLayout.MediumSpacing };

    public RateCard(string heading, string field)
    {
        this.field = field;
        Background = UiTheme.Panel;
        BorderBrush = UiTheme.CardBorder;
        BorderThickness = UiLayout.CardBorderThickness;
        CornerRadius = new CornerRadius(UiLayout.CardRadius);
        Padding = UiLayout.RatePadding;
        MinHeight = UiLayout.MinimumRateHeight;
        var stack = new StackPanel { Spacing = UiLayout.MediumSpacing };
        stack.Children.Add(Label(heading, UiTypography.Tiny, UiTheme.Muted));
        stack.Children.Add(lines);
        Child = stack;
        ShowEmpty();
    }

    // history: captions give the time of the range peak (or reading) instead of its age.
    public void Update(MetricView[] metrics, bool history = false)
    {
        // Most recently updated sources, then a stable order by topic.
        var sources = metrics.Where(item => item.Name == field)
            .GroupBy(item => item.Topic)
            .Select(topic => topic.MaxBy(item => item.ReceivedTime)!)
            .OrderByDescending(item => item.ReceivedTime)
            .Take(UiLayout.RateCardSources)
            .OrderBy(item => item.Topic, StringComparer.Ordinal)
            .ToArray();
        if (sources.Length == 0)
        {
            ShowEmpty();
            return;
        }
        lines.Children.Clear();
        var size = sources.Length == 1 ? UiTypography.Rate : UiTypography.RateCompact;
        foreach (var metric in sources)
        {
            var peak = metric.RatePeak;
            var number = peak?.Value ?? Telemetry.Number(metric.Latest);
            var line = new StackPanel();
            line.Children.Add(Label(number.HasValue
                ? Telemetry.Format(number.Value / ReportUnits.RateDivisor) + UiText.RateSuffix : UiText.EmptyValue, size));
            var source = DisplayText.ShortTopic(metric.Topic);
            var caption = Label(history
                ? string.Format(peak == null ? UiText.ValueAtFormat : UiText.PeakAtFormat, source, Clock(peak?.ReceivedTime ?? metric.ReceivedTime))
                : source + UiText.SourceAgeSeparator + (peak == null ? metric.Age : Telemetry.Age(peak.ReceivedTime)) + UiText.AgeSuffix,
                UiTypography.Small, UiTheme.Muted);
            caption.TextTrimming = TextTrimming.CharacterEllipsis;
            line.Children.Add(caption);
            lines.Children.Add(line);
        }
    }

    private static string Clock(double time) => DateTimeOffset.FromUnixTimeMilliseconds((long)(time * TelemetryDefaults.MillisecondsPerSecond))
        .ToLocalTime().ToString(TelemetryDefaults.ClockFormat);

    private void ShowEmpty()
    {
        lines.Children.Clear();
        var line = new StackPanel();
        line.Children.Add(Label(UiText.EmptyValue, UiTypography.Rate));
        line.Children.Add(Label(UiText.NoSamples, UiTypography.Small, UiTheme.Muted));
        lines.Children.Add(line);
    }
}
