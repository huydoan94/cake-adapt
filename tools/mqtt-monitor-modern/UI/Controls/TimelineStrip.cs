using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.TextFormatting;

namespace CakeAdaptMonitor;

// The whole loaded history with a shaded range between two handles. Dragging a
// handle moves that edge; dragging the shaded part pans the range.
public sealed class TimelineStrip : Control
{
    public double DataStart { get; set; }
    public double DataEnd { get; set; } = 1;
    public double RangeStart { get; set; }
    public double RangeEnd { get; set; } = 1;
    public (string Topic, Sample[] Points)[] Overview { get; set; } = [];
    public BloatView[] Bloat { get; set; } = [];
    public event Action<double, double>? RangeChanged;
    private TimelineHit dragging;
    private double dragX, dragStart, dragEnd;

    public double PlotWidth => Math.Max(1, Bounds.Width - 2 * UiLayout.StripPadding);
    private double Span => Math.Max(1, DataEnd - DataStart);
    public double XAt(double time) => UiLayout.StripPadding + (time - DataStart) / Span * PlotWidth;

    public TimelineStrip()
    {
        Height = UiLayout.StripHeight;
        ClipToBounds = true;
        PointerPressed += (_, e) =>
        {
            var x = e.GetPosition(this).X;
            dragging = Hit(x);
            if (dragging == TimelineHit.None)
                return;
            (dragX, dragStart, dragEnd) = (x, RangeStart, RangeEnd);
            e.Pointer.Capture(this);
        };
        PointerMoved += (_, e) =>
        {
            if (dragging == TimelineHit.None)
                return;
            var delta = (e.GetPosition(this).X - dragX) / PlotWidth * Span;
            (RangeStart, RangeEnd) = TimelineDrag.Apply(dragging, dragStart, dragEnd, delta, DataStart, DataEnd, HistoryModeDefaults.MinimumRangeSeconds);
            InvalidateVisual();
            RangeChanged?.Invoke(RangeStart, RangeEnd);
        };
        PointerReleased += (_, e) =>
        {
            dragging = TimelineHit.None;
            e.Pointer.Capture(null);
        };
    }

    private TimelineHit Hit(double x)
    {
        double start = XAt(RangeStart), end = XAt(RangeEnd), reach = UiLayout.HandleHitWidth / 2;
        if (Math.Abs(x - start) <= reach)
            return TimelineHit.Start;
        if (Math.Abs(x - end) <= reach)
            return TimelineHit.End;
        return x > start && x < end ? TimelineHit.Range : TimelineHit.None;
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        var height = Bounds.Height;
        context.FillRectangle(UiTheme.Panel, new Rect(Bounds.Size));
        var traceTop = UiLayout.StripLabelHeight;
        var traceBottom = height - UiLayout.StripBloatHeight - UiLayout.StripTraceGap;
        for (var i = 0; i < Overview.Length; i++)
        {
            var points = Overview[i].Points.Where(p => p.Value.HasValue).ToArray();
            if (points.Length < 2)
                continue;
            var top = Math.Max(1, points.Max(p => p.Value!.Value));
            var geometry = new StreamGeometry();
            using (var path = geometry.Open())
            {
                path.BeginFigure(new Point(XAt(points[0].SourceTime), traceBottom - points[0].Value!.Value / top * (traceBottom - traceTop)), false);
                foreach (var point in points.Skip(1))
                    path.LineTo(new Point(XAt(point.SourceTime), traceBottom - point.Value!.Value / top * (traceBottom - traceTop)));
                path.EndFigure(false);
            }
            context.DrawGeometry(null, new Pen(new SolidColorBrush(UiTheme.Palette[i % UiTheme.Palette.Length]), UiLayout.StripStroke), geometry);
        }
        var topics = Bloat.Select(view => view.Topic).Distinct().ToList();
        foreach (var view in Bloat)
        {
            var pen = new Pen(new SolidColorBrush(UiTheme.BloatPalette[topics.IndexOf(view.Topic) % UiTheme.BloatPalette.Length]), UiLayout.StripStroke);
            foreach (var bucket in view.Buckets.Where(bucket => bucket.Count > 0))
                context.DrawLine(pen, new Point(XAt(bucket.Time), height - UiLayout.StripBloatHeight), new Point(XAt(bucket.Time), height));
        }
        var showDate = TimeTicks.CrossesMidnight(DataStart, DataEnd);
        var step = TimeTicks.Step(Span);
        foreach (var tick in TimeTicks.Times(DataStart, DataEnd))
            new TextLayout(TimeTicks.Label(tick, step, showDate), new Typeface(UiTheme.Font), UiTypography.Tiny, UiTheme.AxisText)
                .Draw(context, new Point(XAt(tick), 0));
        double start = XAt(RangeStart), end = XAt(RangeEnd);
        context.FillRectangle(UiTheme.StripOutside, new Rect(0, 0, Math.Max(0, start), height));
        context.FillRectangle(UiTheme.StripOutside, new Rect(end, 0, Math.Max(0, Bounds.Width - end), height));
        context.FillRectangle(UiTheme.StripRange, new Rect(start, 0, Math.Max(0, end - start), height));
        context.FillRectangle(UiTheme.StripHandle, new Rect(start - UiLayout.HandleWidth / 2, 0, UiLayout.HandleWidth, height));
        context.FillRectangle(UiTheme.StripHandle, new Rect(end - UiLayout.HandleWidth / 2, 0, UiLayout.HandleWidth, height));
    }
}
