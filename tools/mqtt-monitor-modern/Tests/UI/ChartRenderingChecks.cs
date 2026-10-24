using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Media;
using Avalonia.VisualTree;
using Avalonia.Threading;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void ChartRenderingChecks(MainWindow window, string path)
    {
        var chart = window.GetVisualDescendants().OfType<TrendChart>().First();
        var axisEnd = chart.EndTime;
        Thread.Sleep(20);
        window.Refresh();
        Check(chart.EndTime > axisEnd, "chart time axis advances without new data");
        var bridgeTime = Telemetry.Now;
        var bridgeChart = new TrendChart
        {
            Seconds = 300,
            EndTime = bridgeTime,
            Lines = [new PlotLine("Bridge test",
            [new Sample(bridgeTime - 240, bridgeTime, 50), new Sample(bridgeTime - 150, bridgeTime, null),
             new Sample(bridgeTime - 60, bridgeTime, 50)], TrendChart.Palette[0])]
        };
        var bridgeWindow = new Window { Width = 500, Height = 300, Background = Brushes.Black, Content = bridgeChart };
        bridgeWindow.Show(window);
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var bridgeBitmap = bridgeWindow.CaptureRenderedFrame();
        var pointer = System.Runtime.InteropServices.Marshal.AllocHGlobal(36);
        try
        {
            var x = (int)(64 + (bridgeChart.Bounds.Width - 82) / 2);
            bridgeBitmap!.CopyPixels(new PixelRect(x - 1, 14, 3, 3), pointer, 36, 12);
            var pixels = new byte[36];
            System.Runtime.InteropServices.Marshal.Copy(pointer, pixels, 0, pixels.Length);
            var rgba = bridgeBitmap.Format == Avalonia.Platform.PixelFormat.Rgba8888;
            var blueLine = Enumerable.Range(0, 9).Any(i => pixels[i * 4 + (rgba ? 2 : 0)] > 160 &&
                pixels[i * 4 + (rgba ? 2 : 0)] - pixels[i * 4 + (rgba ? 0 : 2)] > 40);
            Check(blueLine, "rendered line bridges a long gap and invalid reading");
        }
        finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(pointer); }
        bridgeBitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "bridge-preview.png"));
        bridgeWindow.Close();
    }

}
