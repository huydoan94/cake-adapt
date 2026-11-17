using Avalonia;

namespace CakeAdaptMonitor;

public sealed record PlotColumn(double X, double Top, double Bottom, double Mean, int Count);

// Reduce projected points to one column per few pixels: the highest and lowest
// screen positions keep every spike, and the mean gives a readable line.
public static class PlotEnvelope
{
    public static PlotColumn[] Reduce(Point[] points, double width)
    {
        if (points.Length == 0)
            return [];
        var left = points[0].X;
        return points.GroupBy(point => (long)Math.Floor((point.X - left) / width))
            .OrderBy(column => column.Key)
            .Select(column => new PlotColumn(column.Average(p => p.X), column.Min(p => p.Y), column.Max(p => p.Y),
                column.Average(p => p.Y), column.Count()))
            .ToArray();
    }

    // Draw an envelope only when columns hold several points on average; sparse
    // traces keep their exact line.
    public static bool Dense(int points, int columns) => columns > 0 && points >= columns * ChartLayout.EnvelopeDensity;
}
