using Avalonia.Controls;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void DashboardChecks(MainWindow window, MetricStore store)
    {
        store.PublishBatch();
        var received = Telemetry.Now;
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {received:R}; 100; 85000; 6500; 180000; 25000", received);
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {received:R}; 100; 40; 30; 180000; 25000", received);
        store.PublishBatch();
        window.Refresh();
        Pump();
        string CardValue(string label)
        {
            var heading = window.GetVisualDescendants().OfType<TextBlock>().Single(t => t.Text == label);
            return ((StackPanel)heading.Parent!).Children.OfType<TextBlock>().ElementAt(1).Text!;
        }
        Check(CardValue("DOWNLOAD") == "85 Mbit/s" && CardValue("UPLOAD") == "6.5 Mbit/s",
            "Download and Upload labels stay unchanged while cards display interval peaks");
        Check(CardValue("CAKE DOWNLOAD") == "180 Mbit/s" && CardValue("CAKE UPLOAD") == "25 Mbit/s",
            "CAKE cards continue displaying configured rates");
    }

}
