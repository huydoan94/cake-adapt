using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.TextFormatting;

namespace CakeAdaptMonitor;

// Paint only. Series preparation and chart/report choices live outside this control.
public sealed class TrendChart : Control
{
    public static readonly Color[] Palette = UiTheme.Palette;
    public PlotLine[] Lines { get; set; } = [];
    public double Seconds { get; set; } = ChartDefaults.DefaultWindowSeconds;
    public string Unit { get; set; } = "";
    // Optional top of the value scale chosen by PlotSeries.Ceiling; null shows the full range.
    public double? Ceiling { get; set; }
    public double EndTime { get; set; } = Telemetry.Now;
    public string EmptyMessage { get; set; } = UiText.Waiting;
    public string EmptyHint { get; set; } = UiText.ConnectionHelp;
    private Point? hover;

    public TrendChart()
    {
        MinHeight = ChartLayout.MinimumHeight;
        ClipToBounds = true;
        PointerMoved += (_, e) =>
        {
            hover = e.GetPosition(this);
            InvalidateVisual();
        };
        PointerExited += (_, _) =>
        {
            hover = null;
            InvalidateVisual();
        };
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        var points = Lines.SelectMany(line => line.Points).Where(point => point.Value.HasValue).ToArray();
        if (points.Length == 0)
        {
            DrawEmpty(context);
            return;
        }
        var bounds = new Rect(ChartLayout.PlotLeft, ChartLayout.PlotTop,
            Math.Max(1, Bounds.Width - ChartLayout.HorizontalInset), Math.Max(1, Bounds.Height - ChartLayout.VerticalInset));
        var scale = new PlotScale(bounds, EndTime, Seconds, points, Ceiling);
        DrawAxes(context, scale);
        using (context.PushClip(bounds))
            foreach (var line in Lines)
                DrawTrace(context, scale, line);
        DrawClipped(context, scale);
        if (hover is { } mouse && bounds.Contains(mouse))
            DrawHover(context, scale, mouse);
    }

    private void DrawEmpty(DrawingContext context)
    {
        Text(context, EmptyMessage, Math.Max(ChartLayout.EmptyMinimumLeft, Bounds.Width / 2 - ChartLayout.EmptyTitleOffset),
            Bounds.Height / 2 - ChartLayout.EmptyTitleTop, UiTheme.AxisText, UiTypography.Body);
        Text(context, EmptyHint, Math.Max(ChartLayout.EmptyMinimumLeft, Bounds.Width / 2 - ChartLayout.EmptyHintOffset),
            Bounds.Height / 2 + ChartLayout.EmptyHintTop, UiTheme.AxisText, UiTypography.Caption);
    }

    private void DrawAxes(DrawingContext context, PlotScale scale)
    {
        var plot = scale.Bounds;
        for (var i = 0; i <= ChartLayout.AxisIntervals; i++)
        {
            var fraction = (double)i / ChartLayout.AxisIntervals;
            var y = plot.Bottom - fraction * plot.Height;
            context.DrawLine(new Pen(UiTheme.GridLine, ChartLayout.GridStroke), new Point(plot.Left, y), new Point(plot.Right, y));
            Text(context, Telemetry.Format(scale.ValueAt(fraction)), ChartLayout.AxisLabelLeft, y - ChartLayout.AxisLabelOffset, UiTheme.AxisText);
            var stamp = scale.TimeAt(fraction);
            var label = DateTimeOffset.FromUnixTimeSeconds((long)Math.Clamp(stamp, 0, TelemetryDefaults.MaximumUnixSeconds))
                .ToLocalTime().ToString(TelemetryDefaults.ClockFormat);
            var x = plot.Left + plot.Width * fraction - (i == ChartLayout.AxisIntervals ? ChartLayout.ClockRightOffset : 0);
            Text(context, label, x, plot.Bottom + ChartLayout.ClockBottomOffset, UiTheme.AxisText);
        }
        var clipped = Lines.Any(line => line.Points.Any(scale.AboveTop));
        Text(context, clipped ? Unit + UiText.ClippedSuffix : Unit, ChartLayout.AxisLabelLeft, 0, UiTheme.AxisText);
    }

    private static void DrawTrace(DrawingContext context, PlotScale scale, PlotLine line)
    {
        // Intentionally join valid readings across gaps, as requested by the user.
        var points = line.Points.Select(scale.Project).OfType<Point>().ToArray();
        var brush = new SolidColorBrush(line.Color);
        if (points.Length == 1)
        {
            context.DrawEllipse(brush, null, points[0], ChartLayout.PointRadius, ChartLayout.PointRadius);
            return;
        }
        var columns = PlotEnvelope.Reduce(points, ChartLayout.EnvelopeColumnWidth);
        if (PlotEnvelope.Dense(points.Length, columns.Length))
        {
            // Band from each column's lowest to highest reading keeps every spike visible.
            var band = new StreamGeometry();
            using (var path = band.Open())
            {
                path.BeginFigure(new Point(columns[0].X, columns[0].Top), true);
                foreach (var column in columns.Skip(1))
                    path.LineTo(new Point(column.X, column.Top));
                foreach (var column in columns.Reverse())
                    path.LineTo(new Point(column.X, column.Bottom));
                path.EndFigure(true);
            }
            context.DrawGeometry(new SolidColorBrush(Color.FromArgb(ChartLayout.EnvelopeAlpha, line.Color.R, line.Color.G, line.Color.B)), null, band);
            points = columns.Select(column => new Point(column.X, column.Mean)).ToArray();
        }
        if (points.Length > 1)
            context.DrawGeometry(null, new Pen(brush, ChartLayout.TraceStroke), Polyline(points));
    }

    private static StreamGeometry Polyline(Point[] points)
    {
        var geometry = new StreamGeometry();
        using var path = geometry.Open();
        path.BeginFigure(points[0], false);
        foreach (var point in points.Skip(1))
            path.LineTo(point);
        path.EndFigure(false);
        return geometry;
    }

    // A small marker at the top edge for each column holding a reading above a clipped scale.
    private void DrawClipped(DrawingContext context, PlotScale scale)
    {
        var top = scale.Bounds.Top;
        foreach (var line in Lines)
        {
            var brush = new SolidColorBrush(line.Color);
            var spikes = line.Points.Where(scale.AboveTop).Select(scale.Project).OfType<Point>()
                .GroupBy(point => Math.Floor(point.X / ChartLayout.EnvelopeColumnWidth)).Select(column => column.First().X);
            foreach (var x in spikes)
            {
                var marker = new StreamGeometry();
                using (var path = marker.Open())
                {
                    path.BeginFigure(new Point(x - ChartLayout.ClipMarkerSize, top + ChartLayout.ClipMarkerSize), true);
                    path.LineTo(new Point(x, top));
                    path.LineTo(new Point(x + ChartLayout.ClipMarkerSize, top + ChartLayout.ClipMarkerSize));
                    path.EndFigure(true);
                }
                context.DrawGeometry(brush, null, marker);
            }
        }
    }

    private void DrawHover(DrawingContext context, PlotScale scale, Point mouse)
    {
        var plot = scale.Bounds;
        context.DrawLine(new Pen(UiTheme.Crosshair, ChartLayout.CrosshairStroke), new Point(mouse.X, plot.Top), new Point(mouse.X, plot.Bottom));
        var stamp = scale.TimeAt(mouse);
        var labels = new List<string>();
        foreach (var line in Lines)
        {
            var nearest = line.Points.Where(point => point.Value.HasValue).MinBy(point => Math.Abs(point.SourceTime - stamp));
            if (nearest != null && Math.Abs(nearest.SourceTime - stamp) < Math.Max(ChartLayout.HoverToleranceSeconds, Seconds / ChartLayout.HoverWindowDivisor))
                labels.Add(line.Label + UiText.HoverSeparator + Telemetry.Format(nearest.Value!.Value));
        }
        if (labels.Count == 0)
            return;
        var tooltip = new Rect(Math.Clamp(mouse.X + ChartLayout.TooltipOffset, plot.Left, Math.Max(plot.Left, plot.Right - ChartLayout.TooltipRightInset)),
            plot.Top + ChartLayout.TooltipTop, ChartLayout.TooltipWidth, labels.Count * ChartLayout.TooltipLineHeight + ChartLayout.TooltipPadding);
        context.DrawRectangle(UiTheme.TooltipBackground, new Pen(UiTheme.TooltipBorder, ChartLayout.TooltipBorderStroke), tooltip, ChartLayout.TooltipRadius, ChartLayout.TooltipRadius);
        for (var i = 0; i < labels.Count; i++)
            Text(context, labels[i], tooltip.X + ChartLayout.TooltipTextLeft, tooltip.Y + ChartLayout.TooltipTextTop + i * ChartLayout.TooltipLineHeight, UiTheme.TooltipForeground);
    }

    private static void Text(DrawingContext context, string text, double x, double y, IBrush brush, double size = UiTypography.Small)
    {
        var layout = new TextLayout(text, new Typeface(UiTheme.Font), size, brush);
        layout.Draw(context, new Point(x, y));
    }
}
