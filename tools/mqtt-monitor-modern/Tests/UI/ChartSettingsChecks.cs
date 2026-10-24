using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using CakeAdaptMonitor;

static partial class Tests
{
    private static void ChartSettingsFlow(MainWindow window, MetricStore store, string path)
    {
        var panel = window.GetVisualDescendants().OfType<ChartPanel>().First();
        var chart = panel.GetVisualDescendants().OfType<TrendChart>().Single();
        ChartSettingsDialog Open()
        {
            panel.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Settings"))
                .RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Pump();
            return window.OwnedWindows.OfType<ChartSettingsDialog>().Single();
        }
        var dialog = Open();
        using (var bitmap = dialog.CaptureRenderedFrame())
            bitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "settings-preview.png"));
        var choices = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(choices.Length == 4, "chart settings exposes current throughput traces");
        foreach (var choice in choices)
            choice.IsChecked = ((TraceKey)choice.Tag!).Name == "DL_ACHIEVED_RATE_KBPS";
        Click(dialog, "Apply");
        Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Label.StartsWith("DL traffic"), "applying settings draws only the chosen trace");
        var now = Telemetry.Now;
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {now:R}; 1; 54321; 5000; 180000; 25000", now);
        window.Refresh();
        Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Points.Any(p => p.Value == 54.321), "selection survives updates and record changes");
        panel.Select("CPU");
        window.Refresh();
        Pump();
        panel.Select("Throughput");
        window.Refresh();
        Pump();
        Check(chart.Lines.Length == 1, "trace choices are remembered per preset in memory");
        dialog = Open();
        Click(dialog, "Clear");
        Click(dialog, "Apply");
        Pump();
        Check(chart.Lines.Length == 0 && chart.EmptyMessage.Contains("selected"), "clearing traces gives an explicit empty selection");
        dialog = Open();
        Click(dialog, "Use defaults");
        Pump();
        Check(chart.Lines.Length == 4, "defaults restore automatic traces");
        store.Accept("cake-adapt/sample/second", $"LOAD; date; {now:R}; 1; 30000; 5000; 150000; 25000", now);
        window.Refresh();
        Pump();
        dialog = Open();
        Click(dialog, "Select all");
        Click(dialog, "Apply");
        Pump();
        Check(chart.Lines.Length == 8, "settings can explicitly draw more than the automatic six traces");
        dialog = Open();
        Click(dialog, "Clear");
        dialog.Close();
        Pump();
        Check(chart.Lines.Length == 8, "closing settings without applying preserves the plot");
        var source = window.GetVisualDescendants().OfType<ComboBox>()
            .Single(c => c.Items.OfType<string>().Contains("All sources"));
        source.SelectedItem = "cake-adapt/sample/main";
        window.Refresh();
        Pump();
        dialog = Open();
        choices = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(choices.Length == 4 && choices.All(c => ((TraceKey)c.Tag!).Topic == "cake-adapt/sample/main"),
            "settings only lists the selected topic");
        Check(choices.All(c => c.IsChecked == true), "a topic starts with its own automatic choices");
        foreach (var choice in choices)
            choice.IsChecked = ((TraceKey)choice.Tag!).Name == "DL_ACHIEVED_RATE_KBPS";
        Click(dialog, "Apply");
        Pump();
        source.SelectedItem = "cake-adapt/sample/second";
        window.Refresh();
        Pump();
        Check(chart.Lines.Length == 4, "switching topics does not reuse another topic selection");
        dialog = Open();
        foreach (var choice in dialog.GetVisualDescendants().OfType<CheckBox>())
            choice.IsChecked = ((TraceKey)choice.Tag!).Name == "UL_ACHIEVED_RATE_KBPS";
        Click(dialog, "Apply");
        Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Label.StartsWith("UL traffic"), "second topic can have an independent selection");
        source.SelectedItem = "cake-adapt/sample/main";
        window.Refresh();
        Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Label.StartsWith("DL traffic"), "switching back restores the topic selection");
        store.Accept("cake-adapt/sample/main", "COPY_HEADER; LOG_TIMESTAMP; REFLECTOR; CAKE_DL_RATE_KBPS\n" +
            $"COPY; {now:R}; sample-reflector; 180000", now);
        window.Refresh();
        Pump();
        dialog = Open();
        choices = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(choices.Length == 4 && choices.All(c => ((TraceKey)c.Tag!).Dimension.Length == 0),
            "global fields do not repeat for every reflector");
        Check(choices.Count(c => c.IsChecked == true) == 1, "settings reflects the restored topic selection");
        dialog.Width = 460;
        dialog.Height = 350;
        Pump();
        var apply = dialog.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Apply"));
        var position = apply.TranslatePoint(default, dialog);
        Check(position.HasValue && position.Value.Y >= 0 && position.Value.Y + apply.Bounds.Height <= dialog.ClientSize.Height,
            "settings actions remain visible at minimum window size");
        using (var bitmap = dialog.CaptureRenderedFrame())
            bitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "settings-compact-preview.png"));
        dialog.Close();
        Pump();
        source.SelectedItem = "All sources";
        window.Refresh();
        Pump();
        Check(chart.Lines.Length == 8, "All sources retains its separate selection");
        dialog = Open();
        Click(dialog, "Use defaults");
        Pump();
    }


}
