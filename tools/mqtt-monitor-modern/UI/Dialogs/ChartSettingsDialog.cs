using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace CakeAdaptMonitor;

public sealed class ChartSettingsDialog : Window
{
    private readonly List<CheckBox> choices = [];
    private readonly TextBlock status = new() { Foreground = UiTheme.SelectionCount, FontSize = UiTypography.Caption, Margin = UiLayout.SectionBottomMargin };
    private readonly Button apply = new() { Content = UiText.Apply, Background = UiTheme.Primary, Margin = UiLayout.ApplyButtonMargin };

    public ChartSettingsDialog(string report, string topic, MetricView[] candidates, HashSet<TraceKey> selected)
    {
        Title = report + UiText.SettingsSuffix;
        Width = UiLayout.SettingsWidth;
        Height = UiLayout.SettingsHeight;
        MinWidth = UiLayout.MinimumSettingsWidth;
        MinHeight = UiLayout.MinimumSettingsHeight;
        Background = UiTheme.Panel;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new Grid { RowDefinitions = new RowDefinitions(UiLayout.SettingsRows), Margin = UiLayout.SettingsPadding };
        root.Children.Add(new TextBlock { Text = UiText.DrawPrefix + report, FontSize = UiTypography.DialogHeading, Margin = UiLayout.SettingsHeadingMargin });
        var info = new TextBlock
        {
            Text = UiText.SettingsHelp,
            TextWrapping = TextWrapping.Wrap,
            Foreground = UiTheme.Help,
            FontSize = UiTypography.Body,
            Margin = UiLayout.SectionBottomMargin
        };
        Grid.SetRow(info, 1);
        root.Children.Add(info);
        var scope = new TextBlock
        {
            Text = UiText.TopicPrefix + topic,
            TextWrapping = TextWrapping.Wrap,
            Foreground = UiTheme.Link,
            FontSize = UiTypography.Body,
            Margin = UiLayout.RatesMargin
        };
        Grid.SetRow(scope, 2);
        root.Children.Add(scope);
        var list = CreateChoices(candidates, selected);
        var scroller = new ScrollViewer
        {
            Content = list,
            Padding = UiLayout.OptionsScrollPadding,
            HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled,
            Margin = UiLayout.SectionBottomMargin
        };
        Grid.SetRow(scroller, 3);
        root.Children.Add(scroller);
        Grid.SetRow(status, 4);
        root.Children.Add(status);
        var buttons = CreateActions();
        Grid.SetRow(buttons, 5);
        root.Children.Add(buttons);
        Content = root;
    }
    private StackPanel CreateChoices(MetricView[] candidates, HashSet<TraceKey> selected)
    {
        var list = new StackPanel { Spacing = UiLayout.ItemSpacing };
        foreach (var metric in candidates.OrderBy(m => m.Name).ThenBy(m => m.Topic).ThenBy(m => m.Dimension))
        {
            var key = TraceKey.From(metric);
            var text = new StackPanel { Spacing = UiLayout.TinySpacing, Margin = UiLayout.OptionMargin };
            text.Children.Add(new TextBlock { Text = metric.Name.Replace(UiText.FieldSeparator, UiText.DisplaySeparator), FontSize = UiTypography.Body, TextWrapping = TextWrapping.Wrap });
            text.Children.Add(new TextBlock
            {
                Text = metric.Topic + (metric.Dimension.Length > 0 ? UiText.SourceSeparator + metric.Dimension : "") +
                UiText.SourceSeparator + metric.Latest + UiText.DisplaySeparator + metric.Unit,
                FontSize = UiTypography.Small,
                Foreground = UiTheme.MetricSource,
                TextWrapping = TextWrapping.Wrap
            });
            var option = new CheckBox
            {
                Content = text,
                Tag = key,
                IsChecked = selected.Contains(key),
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch
            };
            choices.Add(option);
            list.Children.Add(option);
        }
        if (choices.Count == 0)
            list.Children.Add(new TextBlock { Text = UiText.NoOptions, TextWrapping = TextWrapping.Wrap });
        return list;
    }

    private WrapPanel CreateActions()
    {
        var buttons = new WrapPanel { Orientation = Orientation.Horizontal };
        var all = new Button { Content = choices.Count > ChartDefaults.MaximumTraces ? UiText.FirstTraces : UiText.SelectAll, Margin = UiLayout.DialogButtonMargin };
        var clear = new Button { Content = UiText.Clear, Margin = UiLayout.DialogButtonMargin };
        var defaults = new Button { Content = UiText.Defaults, Margin = UiLayout.DialogButtonMargin };
        all.Click += (_, _) => SelectFirst(ChartDefaults.MaximumTraces);
        clear.Click += (_, _) => SelectFirst(0);
        defaults.Click += (_, _) => Close(new ChartSelection(true, []));
        apply.Click += (_, _) => Close(new ChartSelection(false, choices.Where(c => c.IsChecked == true).Select(c => (TraceKey)c.Tag!).ToHashSet()));
        foreach (var option in choices)
            option.IsCheckedChanged += (_, _) => UpdateCount();
        UpdateCount();
        buttons.Children.Add(all);
        buttons.Children.Add(clear);
        buttons.Children.Add(defaults);
        buttons.Children.Add(apply);
        return buttons;
    }

    private void SelectFirst(int limit)
    {
        for (var i = 0; i < choices.Count; i++)
            choices[i].IsChecked = i < limit;
    }

    private void UpdateCount()
    {
        var count = choices.Count(c => c.IsChecked == true);
        status.Text = string.Format(UiText.SelectionFormat, count, choices.Count) + (count > ChartDefaults.MaximumTraces ? UiText.SelectionLimit : "");
        apply.IsEnabled = count <= ChartDefaults.MaximumTraces;
    }
}
