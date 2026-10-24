using Avalonia;

namespace CakeAdaptMonitor;

// Coordinate conversion, normalized first so extreme finite values do not overflow.
internal sealed class PlotScale
{
    public Rect Bounds
    {
        get;
    }
    private readonly double start;
    private readonly double seconds;
    private readonly double magnitude;
    private readonly double low;
    private readonly double high;

    public PlotScale(Rect bounds, double end, double seconds, Sample[] points)
    {
        Bounds = bounds;
        start = end - seconds;
        this.seconds = seconds;
        magnitude = points.Max(point => Math.Abs(point.Value!.Value));
        if (magnitude == 0)
            magnitude = 1;
        low = Math.Min(0, points.Min(point => point.Value!.Value / magnitude));
        high = Math.Max(0, points.Max(point => point.Value!.Value / magnitude));
        if (high == low)
            high = low + 1;
    }

    public double ValueAt(double fraction) => (low + (high - low) * fraction) * magnitude;
    public double TimeAt(double fraction) => start + seconds * fraction;
    public double TimeAt(Point point) => TimeAt((point.X - Bounds.Left) / Bounds.Width);

    public Point? Project(Sample sample)
    {
        if (!sample.Value.HasValue || sample.SourceTime < start)
            return null;
        var x = Bounds.Left + (sample.SourceTime - start) / seconds * Bounds.Width;
        var y = Bounds.Bottom - (sample.Value.Value / magnitude - low) / (high - low) * Bounds.Height;
        return double.IsFinite(x) && double.IsFinite(y) ? new Point(x, y) : null;
    }
}
