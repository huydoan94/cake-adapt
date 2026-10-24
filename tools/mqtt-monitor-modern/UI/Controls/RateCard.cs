using Avalonia;
using Avalonia.Controls;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

internal sealed class RateCard : Border
{
    private readonly string field;
    private readonly TextBlock value = Label(UiText.EmptyValue, UiTypography.Rate);
    private readonly TextBlock age = Label(UiText.NoSamples, UiTypography.Small, UiTheme.Muted);

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
        stack.Children.Add(value);
        stack.Children.Add(age);
        Child = stack;
    }

    public void Update(MetricView[] metrics)
    {
        var metric = metrics.Where(item => item.Name == field).MaxBy(item => item.ReceivedTime);
        var peak = metric?.RatePeak;
        var number = peak?.Value ?? (metric == null ? null : Telemetry.Number(metric.Latest));
        value.Text = number.HasValue
            ? Telemetry.Format(number.Value / ReportUnits.RateDivisor) + UiText.RateSuffix : UiText.EmptyValue;
        age.Text = metric == null ? UiText.NoSamples :
            (peak == null ? metric.Age : Telemetry.Age(peak.ReceivedTime)) +
            UiText.AgeSourceSeparator + DisplayText.ShortTopic(metric.Topic);
    }
}
