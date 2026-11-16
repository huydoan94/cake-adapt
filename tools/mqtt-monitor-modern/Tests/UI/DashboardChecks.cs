using Avalonia;
using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    // Each card line is a StackPanel of (value, "age · source").
    private static (string Value, string Caption)[] CardLines(Window window, string label)
    {
        var heading = window.GetVisualDescendants().OfType<TextBlock>().Single(t => t.Text == label);
        var lines = ((StackPanel)heading.Parent!).Children.OfType<StackPanel>().Single();
        return lines.Children.OfType<StackPanel>().Select(line =>
        {
            var texts = line.Children.OfType<TextBlock>().ToArray();
            return (texts[0].Text!, texts[1].Text!);
        }).ToArray();
    }

    private static void DashboardChecks(MainWindow window, MetricStore store)
    {
        store.PublishBatch();
        var received = Telemetry.Now;
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {received:R}; 100; 85000; 6500; 180000; 25000", received);
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {received:R}; 100; 40; 30; 180000; 25000", received);
        store.PublishBatch();
        window.Refresh();
        Pump();
        string CardValue(string label) => CardLines(window, label).Single().Value;
        Check(CardValue("DOWNLOAD") == "85 Mbit/s" && CardValue("UPLOAD") == "6.5 Mbit/s",
            "Download and Upload labels stay unchanged while cards display interval peaks");
        Check(CardValue("CAKE DOWNLOAD") == "180 Mbit/s" && CardValue("CAKE UPLOAD") == "25 Mbit/s",
            "CAKE cards continue displaying configured rates");

        // Several routers: one line each, in topic order, at most three.
        var routers = new MetricStore();
        var window2 = new MainWindow { Width = 1440, Height = 920 };
        window2.Preview(routers);
        window2.Show();
        void Load(string topic, double download, double at) =>
            routers.Accept(topic, $"LOAD; date; {at:R}; 100; {download:R}; 1000; 100000; 20000", at);
        Load("cake-adapt/router-b", 20000, received);
        Load("cake-adapt/router-a", 10000, received + 0.1);
        routers.PublishBatch();
        window2.Refresh();
        Pump();
        var two = CardLines(window2, "DOWNLOAD");
        Check(two.Length == 2 && two[0].Caption.EndsWith("cake-adapt/router-a") && two[1].Caption.EndsWith("cake-adapt/router-b") &&
            two[0].Value == "10 Mbit/s" && two[1].Value == "20 Mbit/s",
            "rate cards show one line per router in a stable topic order");
        Load("cake-adapt/router-a", 11000, received + 0.2);
        routers.PublishBatch();
        window2.Refresh();
        Pump();
        var again = CardLines(window2, "DOWNLOAD");
        Check(again[0].Caption.EndsWith("cake-adapt/router-a") && again[1].Caption.EndsWith("cake-adapt/router-b"),
            "a newer reading from one router does not reorder the card lines");
        Load("cake-adapt/router-c", 30000, received + 0.3);
        Load("cake-adapt/router-d", 40000, received + 0.4);
        routers.PublishBatch();
        window2.Refresh();
        Pump();
        var many = CardLines(window2, "DOWNLOAD");
        Check(many.Length == 3 && !many.Any(line => line.Caption.EndsWith("cake-adapt/router-b")),
            "rate cards show at most three routers, the most recently updated ones");
        window2.Close();

        Check(MainWindow.ShouldMaximize(new Size(1280, 760)) && MainWindow.ShouldMaximize(new Size(1366, 728)) &&
            !MainWindow.ShouldMaximize(new Size(1920, 1040)),
            "the window opens maximized when its default size does not fit the screen");
    }

    // At 1280x800 the charts must not run under each other or the status line.
    private static void SmallScreenChecks(MainWindow window)
    {
        window.Width = 1280;
        window.Height = 737;
        Pump();
        Pump();
        foreach (var panel in window.GetVisualDescendants().OfType<ChartPanel>())
        {
            var legend = panel.Children.OfType<WrapPanel>().Single();
            var chart = panel.Children.OfType<TrendChart>().Single();
            Check(legend.Bounds.Bottom <= chart.Bounds.Top + 0.5, "chart legend stays above the plot on a small screen");
        }
        var scroll = window.GetVisualDescendants().OfType<ScrollViewer>()
            .First(viewer => viewer.Content is Grid grid && grid.Children.OfType<Border>().Any(b => b.Child is ChartPanel));
        var status = window.GetVisualDescendants().OfType<TextBlock>().Single(t => t.Text?.Contains("memory history") == true);
        var scrollBottom = scroll.TranslatePoint(new Point(0, scroll.Bounds.Height), window)!.Value.Y;
        var statusTop = status.TranslatePoint(new Point(0, 0), window)!.Value.Y;
        Check(scrollBottom <= statusTop + 0.5, "the status line has its own space below the dashboard");
        Check(scroll.Extent.Height > scroll.Viewport.Height, "the dashboard scrolls when the window is too short");
        window.Width = 1440;
        window.Height = 920;
        Pump();
    }
}
