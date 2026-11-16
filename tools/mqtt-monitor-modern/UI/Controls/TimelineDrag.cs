namespace CakeAdaptMonitor;

public enum TimelineHit { None, Start, End, Range }

// Pure range arithmetic for the timeline strip, kept apart from pointer handling.
public static class TimelineDrag
{
    public static (double Start, double End) Apply(TimelineHit hit, double start, double end, double deltaSeconds,
        double dataStart, double dataEnd, double minimum)
    {
        minimum = Math.Min(minimum, dataEnd - dataStart);
        return hit switch
        {
            TimelineHit.Start => (Math.Clamp(start + deltaSeconds, dataStart, end - minimum), end),
            TimelineHit.End => (start, Math.Clamp(end + deltaSeconds, start + minimum, dataEnd)),
            TimelineHit.Range => Pan(start, end, deltaSeconds, dataStart, dataEnd),
            _ => (start, end)
        };
    }

    private static (double, double) Pan(double start, double end, double delta, double dataStart, double dataEnd)
    {
        var length = end - start;
        var moved = Math.Clamp(start + delta, dataStart, Math.Max(dataStart, dataEnd - length));
        return (moved, moved + length);
    }
}
