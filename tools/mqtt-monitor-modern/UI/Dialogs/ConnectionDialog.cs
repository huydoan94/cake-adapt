using System.Collections.ObjectModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Data;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace CakeAdaptMonitor;

public sealed class ConnectionDialog : Window
{
    public ConnectionDialog(BrokerSettings previous)
    {
        Title = UiText.ConnectionTitle;
        Width = UiLayout.ConnectionWidth;
        Height = UiLayout.ConnectionHeight;
        CanResize = false;
        Background = UiTheme.Panel;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var stack = new StackPanel { Spacing = UiLayout.FormSpacing, Margin = UiLayout.ConnectionPadding };
        stack.Children.Add(new TextBlock { Text = UiText.ConnectionHeading, FontSize = UiTypography.DialogHeading });
        var host = Input(UiText.Host, previous.Host);
        var port = Input(UiText.Port, previous.Port.ToString());
        var topic = Input(UiText.SubscriptionTopic, previous.Topic);
        var username = Input(UiText.Username, previous.Username);
        var password = Input(UiText.Password, "");
        password.PasswordChar = UiLayout.PasswordCharacter;
        var tls = new CheckBox { Content = UiText.Tls, IsChecked = previous.Tls };
        stack.Children.Add(tls);
        var ca = Input(UiText.CaPath, previous.Ca);
        tls.IsCheckedChanged += (_, _) => { if (port.Text is ConnectionDefaults.PlainPortText or ConnectionDefaults.TlsPortText) port.Text = tls.IsChecked == true ? ConnectionDefaults.TlsPortText : ConnectionDefaults.PlainPortText; };
        var error = new TextBlock { Foreground = UiTheme.Error, TextWrapping = TextWrapping.Wrap };
        stack.Children.Add(error);
        var submit = new Button
        {
            Content = UiText.Connect,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            Background = UiTheme.Primary
        };
        submit.Click += (_, _) =>
        {
            try
            {
                if (!int.TryParse(port.Text, out var portNumber))
                    throw new ArgumentException(UiText.InvalidPort);
                var settings = new BrokerSettings
                {
                    Host = host.Text?.Trim() ?? "",
                    Port = portNumber,
                    Topic = topic.Text?.Trim() ?? "",
                    Username = username.Text ?? "",
                    Password = password.Text ?? "",
                    Tls = tls.IsChecked == true,
                    Ca = ca.Text ?? ""
                };
                settings.Validate();
                Close(settings);
            }
            catch (ArgumentException e) { error.Text = e.Message; }
        };
        stack.Children.Add(submit);
        Content = new ScrollViewer { Content = stack };
        TextBox Input(string hint, string text)
        {
            stack.Children.Add(new TextBlock { Text = hint, FontSize = UiTypography.Caption, Foreground = UiTheme.Muted });
            var box = new TextBox { Text = text };
            stack.Children.Add(box);
            return box;
        }
    }
}
