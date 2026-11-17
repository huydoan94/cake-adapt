using Avalonia.Controls;

namespace CakeAdaptMonitor;

public static class MetricColumns
{
    // Identity columns first, so rows of the same metric from different routers or
    // reflectors are told apart without scrolling. A header needs its text plus about
    // 44 px (theme padding and the sort-arrow slot). Fixed widths fit those and the
    // values at UiTypography.Body; the last column takes the remaining space, so the
    // table fits a maximized 1280-pixel window without scrolling sideways.
    public const double RemainderMinimumWidth = 80;
    public static readonly (string Heading, string Property, DataGridLength Width)[] All = [
        (UiText.MetricHeading, nameof(MetricRow.Name), new(150)), (UiText.TopicHeading, nameof(MetricRow.Topic), new(145)),
        (UiText.DimensionHeading, nameof(MetricRow.Dimension), new(140)), (UiText.LatestHeading, nameof(MetricRow.Latest), new(85)),
        (UiText.UnitHeading, nameof(MetricRow.Unit), new(70)), (UiText.MinHeading, nameof(MetricRow.Min), new(70)),
        (UiText.MeanHeading, nameof(MetricRow.Mean), new(80)), (UiText.MaxHeading, nameof(MetricRow.Max), new(70)),
        (UiText.AgeHeading, nameof(MetricRow.Age), new(70)), (UiText.PointsHeading, nameof(MetricRow.Count), new(85)),
        (UiText.RecordHeading, nameof(MetricRow.Record), new(1, DataGridLengthUnitType.Star))];
}
