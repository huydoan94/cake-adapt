using Avalonia.Controls;
using Avalonia.Layout;
using static CakeAdaptMonitor.UiControls;

namespace CakeAdaptMonitor;

public enum StartChoice { Live, History }

// Shown at program start and after Start over: live broker or saved captures.
public sealed class StartWindow : Window
{
    public StartWindow()
    {
        Title = UiText.StartTitle;
        Width = UiLayout.StartWidth;
        Height = UiLayout.StartHeight;
        CanResize = false;
        Background = UiTheme.Panel;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        DarkTitleBar.Apply(this);
        var form = new StackPanel { Spacing = UiLayout.FormSpacing, Margin = UiLayout.ConnectionPadding };
        form.Children.Add(Label(UiText.StartHeading, UiTypography.DialogHeading));
        foreach (var (choice, text, help) in new[] { (StartChoice.Live, UiText.WatchLive, UiText.WatchLiveHelp), (StartChoice.History, UiText.OpenHistory, UiText.OpenHistoryHelp) })
        {
            var button = ActionButton(text);
            button.HorizontalAlignment = HorizontalAlignment.Stretch;
            button.Click += (_, _) => Close(choice);
            form.Children.Add(button);
            form.Children.Add(Label(help, UiTypography.Caption, UiTheme.Muted));
        }
        Content = form;
    }
}
