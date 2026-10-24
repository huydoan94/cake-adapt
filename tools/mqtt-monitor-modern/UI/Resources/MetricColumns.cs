namespace CakeAdaptMonitor;

public static class MetricColumns
{
    public static readonly (string Heading, string Property, double Width)[] All = [
        (UiText.MetricHeading,nameof(MetricRow.Name),235), (UiText.LatestHeading,nameof(MetricRow.Latest),115),
        (UiText.UnitHeading,nameof(MetricRow.Unit),70), (UiText.MinHeading,nameof(MetricRow.Min),90),
        (UiText.MeanHeading,nameof(MetricRow.Mean),115), (UiText.MaxHeading,nameof(MetricRow.Max),90),
        (UiText.PointsHeading,nameof(MetricRow.Count),65), (UiText.AgeHeading,nameof(MetricRow.Age),75),
        (UiText.TopicHeading,nameof(MetricRow.Topic),210), (UiText.RecordHeading,nameof(MetricRow.Record),100),
        (UiText.DimensionHeading,nameof(MetricRow.Dimension),140)];
}
