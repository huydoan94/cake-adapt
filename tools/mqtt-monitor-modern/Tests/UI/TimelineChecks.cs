using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void TimelineChecks()
    {
        const double day = 86400, hour = 3600;
        var startPast = TimelineDrag.Apply(TimelineHit.Start, 10 * hour, 12 * hour, 5 * hour, 0, day, 60);
        Check(startPast == (12 * hour - 60, 12 * hour), "the start handle stops one minute before the end");
        var endLow = TimelineDrag.Apply(TimelineHit.End, 10 * hour, 12 * hour, -20 * hour, 0, day, 60);
        Check(endLow == (10 * hour, 10 * hour + 60), "the end handle stops one minute after the start");
        var startLow = TimelineDrag.Apply(TimelineHit.Start, 1 * hour, 2 * hour, -5 * hour, 0, day, 60);
        Check(startLow == (0, 2 * hour), "the start handle stops at the beginning of the data");
        var pan = TimelineDrag.Apply(TimelineHit.Range, 20 * hour, 22 * hour, 10 * hour, 0, day, 60);
        Check(pan == (day - 2 * hour, day), "panning keeps the range length and stops at the end of the data");
        Check(TimelineDrag.Apply(TimelineHit.None, 1, 2, 5, 0, day, 60) == (1, 2), "a press outside the range changes nothing");

        var strip = new TimelineStrip { DataStart = 0, DataEnd = day, RangeStart = 10 * hour, RangeEnd = 12 * hour };
        var window = new Window { Width = 1000, Height = 120, Content = strip };
        (double Start, double End)? changed = null;
        strip.RangeChanged += (s, e) => changed = (s, e);
        window.Show();
        Pump();
        var middle = strip.XAt(11 * hour);
        var y = strip.Bounds.Height / 2;
        window.MouseDown(new Point(middle, y), MouseButton.Left);
        window.MouseMove(new Point(middle + 100, y));
        window.MouseUp(new Point(middle + 100, y), MouseButton.Left);
        Pump();
        var shift = 100 / strip.PlotWidth * day;
        Check(changed is { } range && Math.Abs(range.Start - (10 * hour + shift)) < 1 && Math.Abs(range.End - (12 * hour + shift)) < 1,
            "dragging the shaded range pans it by the dragged time");
        var startX = strip.XAt(strip.RangeStart);
        window.MouseDown(new Point(startX, y), MouseButton.Left);
        window.MouseMove(new Point(startX - 50, y));
        window.MouseUp(new Point(startX - 50, y), MouseButton.Left);
        Pump();
        Check(changed is { } widened && widened.Start < 10 * hour + shift - 1 && Math.Abs(widened.End - (12 * hour + shift)) < 1,
            "dragging the start handle moves only the start");
        window.Close();
    }
}
