using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.TextFormatting;

namespace CakeAdaptMonitor;

public sealed record PlotLine(string Label, Sample[] Points, Color Color);

public sealed class TrendChart : Control
{
    public static readonly Color[] Palette = UiTheme.Palette;
    public PlotLine[] Lines { get; set; } = [];
    public double Seconds { get; set; } = ChartDefaults.DefaultWindowSeconds;
    public string Unit { get; set; } = "";
    public double EndTime { get; set; } = Telemetry.Now;
    public string EmptyMessage { get; set; } = UiText.Waiting;
    public string EmptyHint { get; set; } = UiText.ConnectionHelp;
    private Point? hover;
    public TrendChart()
    {
        MinHeight = ChartLayout.MinimumHeight;
        ClipToBounds = true;
        PointerMoved += (_, e) => { hover = e.GetPosition(this); InvalidateVisual(); };
        PointerExited += (_, _) => { hover = null; InvalidateVisual(); };
    }
    private static void Text(DrawingContext context, string text, double x, double y, IBrush brush, double size = UiTypography.Small)
    {
        var layout = new TextLayout(text, new Typeface(UiTheme.Font), size, brush);
        layout.Draw(context, new Point(x, y));
    }
    public override void Render(DrawingContext context)
    {
        base.Render(context);
        var muted = UiTheme.AxisText;
        var plot = new Rect(ChartLayout.PlotLeft, ChartLayout.PlotTop, Math.Max(1, Bounds.Width - ChartLayout.HorizontalInset), Math.Max(1, Bounds.Height - ChartLayout.VerticalInset));
        var all = Lines.SelectMany(l => l.Points).Where(p => p.Value.HasValue).ToArray();
        if (all.Length == 0)
        {
            Text(context, EmptyMessage, Math.Max(ChartLayout.EmptyMinimumLeft, Bounds.Width / 2 - ChartLayout.EmptyTitleOffset), Bounds.Height / 2 - ChartLayout.EmptyTitleTop, muted, UiTypography.Body);
            Text(context, EmptyHint, Math.Max(ChartLayout.EmptyMinimumLeft, Bounds.Width / 2 - ChartLayout.EmptyHintOffset), Bounds.Height / 2 + ChartLayout.EmptyHintTop, muted, UiTypography.Caption);
            return;
        }
        var scale = all.Max(p => Math.Abs(p.Value!.Value));
        if (scale == 0)
            scale = 1;
        var low = Math.Min(0, all.Min(p => p.Value!.Value / scale));
        var high = Math.Max(0, all.Max(p => p.Value!.Value / scale));
        if (high == low)
            high = low + 1;
        var end = EndTime;
        var start = end - Seconds;
        for (var i = 0; i <= ChartLayout.AxisIntervals; i++)
        {
            var f = (double)i / ChartLayout.AxisIntervals;
            var y = plot.Bottom - f * plot.Height;
            context.DrawLine(new Pen(UiTheme.GridLine, ChartLayout.GridStroke), new Point(plot.Left, y), new Point(plot.Right, y));
            Text(context, Telemetry.Format((low + (high - low) * f) * scale), ChartLayout.AxisLabelLeft, y - ChartLayout.AxisLabelOffset, muted);
        }
        Text(context, Unit, ChartLayout.AxisLabelLeft, 0, muted);
        for (var i = 0; i <= ChartLayout.AxisIntervals; i++)
        {
            var stamp = start + Seconds * i / ChartLayout.AxisIntervals;
            var label = DateTimeOffset.FromUnixTimeSeconds((long)Math.Clamp(stamp, 0, TelemetryDefaults.MaximumUnixSeconds)).ToLocalTime().ToString(TelemetryDefaults.ClockFormat);
            Text(context, label, plot.Left + plot.Width * i / ChartLayout.AxisIntervals - (i == ChartLayout.AxisIntervals ? ChartLayout.ClockRightOffset : 0), plot.Bottom + ChartLayout.ClockBottomOffset, muted);
        }
        using (context.PushClip(plot))
        {
            foreach (var line in Lines)
            {
                var points = new List<Point>();
                void Flush()
                {
                    if (points.Count == 1)
                        context.DrawEllipse(new SolidColorBrush(line.Color), null, points[0], ChartLayout.PointRadius, ChartLayout.PointRadius);
                    else if (points.Count > 1)
                    {
                        var geometry = new StreamGeometry();
                        using (var path = geometry.Open())
                        {
                            path.BeginFigure(points[0], false);
                            foreach (var point in points.Skip(1))
                                path.LineTo(point);
                            path.EndFigure(false);
                        }
                        context.DrawGeometry(null, new Pen(new SolidColorBrush(line.Color), ChartLayout.TraceStroke), geometry);
                    }
                    points.Clear();
                }
                foreach (var sample in line.Points)
                {
                    if (sample.SourceTime < start)
                        continue;
                    if (sample.Value.HasValue)
                    {
                        var x = plot.Left + (sample.SourceTime - start) / Seconds * plot.Width;
                        var y = plot.Bottom - (sample.Value.Value / scale - low) / (high - low) * plot.Height;
                        if (double.IsFinite(x) && double.IsFinite(y))
                            points.Add(new Point(x, y));
                    }
                }
                Flush();
            }
        }
        if (hover is { } mouse && plot.Contains(mouse))
        {
            context.DrawLine(new Pen(UiTheme.Crosshair, ChartLayout.CrosshairStroke), new Point(mouse.X, plot.Top), new Point(mouse.X, plot.Bottom));
            var stamp = start + (mouse.X - plot.Left) / plot.Width * Seconds;
            var labels = new List<string>();
            foreach (var line in Lines)
            {
                var nearest = line.Points.Where(p => p.Value.HasValue).MinBy(p => Math.Abs(p.SourceTime - stamp));
                if (nearest != null && Math.Abs(nearest.SourceTime - stamp) < Math.Max(ChartLayout.HoverToleranceSeconds, Seconds / ChartLayout.HoverWindowDivisor))
                    labels.Add(line.Label + UiText.HoverSeparator + Telemetry.Format(nearest.Value!.Value));
            }
            if (labels.Count > 0)
            {
                var tooltip = new Rect(Math.Clamp(mouse.X + ChartLayout.TooltipOffset, plot.Left, Math.Max(plot.Left, plot.Right - ChartLayout.TooltipRightInset)), plot.Top + ChartLayout.TooltipTop, ChartLayout.TooltipWidth, labels.Count * ChartLayout.TooltipLineHeight + ChartLayout.TooltipPadding);
                context.DrawRectangle(UiTheme.TooltipBackground, new Pen(UiTheme.TooltipBorder, ChartLayout.TooltipBorderStroke), tooltip, ChartLayout.TooltipRadius, ChartLayout.TooltipRadius);
                for (var i = 0; i < labels.Count; i++)
                    Text(context, labels[i], tooltip.X + ChartLayout.TooltipTextLeft, tooltip.Y + ChartLayout.TooltipTextTop + i * ChartLayout.TooltipLineHeight, UiTheme.TooltipForeground);
            }
        }
    }
}
