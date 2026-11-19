using Avalonia;

namespace CakeAdaptMonitor;

public sealed record PlotColumn(double X, double Top, double Bottom, double Mean, int Count);

// Reduce projected readings to one column per fixed clock window (one second unless
// that is narrower than a pixel): the highest and lowest screen positions keep every
// spike, and the mean gives a readable line. Windows are aligned to the clock, not to
// the first visible reading, so a scrolling chart keeps each reading in its column.
public static class PlotEnvelope
{
    public static PlotColumn[] Reduce(IReadOnlyList<(double Time, Point Point)> readings, double step)
    {
        if (readings.Count == 0)
            return [];
        return readings.GroupBy(reading => (long)Math.Floor(reading.Time / step))
            .OrderBy(column => column.Key)
            .Select(column => new PlotColumn(column.Average(r => r.Point.X), column.Min(r => r.Point.Y), column.Max(r => r.Point.Y),
                column.Average(r => r.Point.Y), column.Count()))
            .ToArray();
    }

    // The shortest window, in seconds, at least EnvelopeMinimumColumnWidth pixels wide
    // when seconds are drawn across width pixels.
    public static double Step(double seconds, double width)
    {
        var perPixel = seconds / Math.Max(1, width);
        foreach (var step in ChartLayout.EnvelopeSteps)
            if (step >= perPixel * ChartLayout.EnvelopeMinimumColumnWidth)
                return step;
        return ChartLayout.EnvelopeSteps[^1];
    }
}
