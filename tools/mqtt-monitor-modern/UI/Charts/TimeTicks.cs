using System.Globalization;

namespace CakeAdaptMonitor;

// Time-axis ticks for a history range: steps from ChartLayout.TickSteps, aligned
// to PC-local clock minutes, hours and days.
public static class TimeTicks
{
    public static double Step(double seconds) =>
        ChartLayout.TickSteps.FirstOrDefault(step => seconds / step <= ChartLayout.MaximumTicks, ChartLayout.TickSteps[^1]);

    public static double[] Times(double start, double end)
    {
        var step = Step(end - start);
        var offset = Offset(start);
        var first = Math.Ceiling((start + offset) / step) * step - offset;
        var ticks = new List<double>();
        for (var tick = first; tick <= end; tick += step)
            ticks.Add(tick);
        return ticks.ToArray();
    }

    public static bool CrossesMidnight(double start, double end) => Local(start).Date != Local(end).Date;

    public static string Label(double time, double step, bool showDate) => Local(time).ToString(
        showDate ? UiText.DateTickFormat : step < ChartLayout.TickSteps[4] ? UiText.SecondTickFormat : UiText.MinuteTickFormat,
        CultureInfo.InvariantCulture);

    private static DateTimeOffset Local(double time) =>
        DateTimeOffset.FromUnixTimeMilliseconds((long)(time * TelemetryDefaults.MillisecondsPerSecond)).ToLocalTime();

    private static double Offset(double time) => Local(time).Offset.TotalSeconds;
}
