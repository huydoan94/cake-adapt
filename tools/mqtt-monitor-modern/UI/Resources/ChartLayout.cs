namespace CakeAdaptMonitor;

public static class ChartLayout
{
    public const double MinimumHeight = 180;
    // History ticks: the smallest step giving at most MaximumTicks labels.
    public static readonly double[] TickSteps = [1, 5, 10, 30, 60, 300, 600, 1800, 3600, 10800, 21600, 43200, 86400];
    public const int MaximumTicks = 8;
    public const double PlotLeft = 64, PlotTop = 15, HorizontalInset = 82, VerticalInset = 48;
    public const int AxisIntervals = 4;
    public const double AxisLabelLeft = 5, AxisLabelOffset = 6, ClockRightOffset = 43, ClockBottomOffset = 12;
    public const double GridStroke = .7, TraceStroke = 1.4, PointRadius = 2.5;
    // Traces: one min/max/mean column per clock window, the shortest of EnvelopeSteps
    // (seconds) at least EnvelopeMinimumColumnWidth pixels wide; the band uses EnvelopeAlpha.
    public static readonly double[] EnvelopeSteps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
    public const double EnvelopeMinimumColumnWidth = 1;
    public const byte EnvelopeAlpha = 70;
    // Latency: clip the scale when the peak exceeds ClipRatio x p99; the top is p99 x ClipHeadroom.
    public const double ClipPercentile = .99, ClipRatio = 3, ClipHeadroom = 1.25, ClipMarkerSize = 4;
    public const double CrosshairStroke = 1, TooltipBorderStroke = 1;
    // Bufferbloat ticks: one row per source, stacked upward from the plot bottom.
    public const double MarkHeight = 6, MarkRowSpacing = 8, MarkStroke = 1.5;
    public const double EmptyMinimumLeft = 20, EmptyTitleOffset = 95, EmptyHintOffset = 101, EmptyTitleTop = 14, EmptyHintTop = 10;
    public const double HoverToleranceSeconds = 5, HoverWindowDivisor = 50;
    public const double TooltipRightInset = 240, TooltipWidth = 235, TooltipLineHeight = 18, TooltipPadding = 14;
    public const double TooltipOffset = 12, TooltipTop = 5, TooltipRadius = 5, TooltipTextLeft = 9, TooltipTextTop = 7;
}
