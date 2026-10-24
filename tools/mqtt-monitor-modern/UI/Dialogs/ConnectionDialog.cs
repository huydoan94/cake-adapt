using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace CakeAdaptMonitor;

public sealed class ConnectionDialog : Window
{
    private readonly StackPanel form = new() { Spacing = UiLayout.FormSpacing, Margin = UiLayout.ConnectionPadding };
    private readonly TextBox host, port, topic, username, password, ca;
    private readonly CheckBox tls;
    private readonly TextBlock error = new() { Foreground = UiTheme.Error, TextWrapping = TextWrapping.Wrap };

    public ConnectionDialog(BrokerSettings previous)
    {
        Title = UiText.ConnectionTitle;
        Width = UiLayout.ConnectionWidth;
        Height = UiLayout.ConnectionHeight;
        CanResize = false;
        Background = UiTheme.Panel;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        form.Children.Add(new TextBlock { Text = UiText.ConnectionHeading, FontSize = UiTypography.DialogHeading });
        host = AddInput(UiText.Host, previous.Host);
        port = AddInput(UiText.Port, previous.Port.ToString());
        topic = AddInput(UiText.SubscriptionTopic, previous.Topic);
        username = AddInput(UiText.Username, previous.Username);
        password = AddInput(UiText.Password, "");
        password.PasswordChar = UiLayout.PasswordCharacter;
        tls = new CheckBox { Content = UiText.Tls, IsChecked = previous.Tls };
        form.Children.Add(tls);
        ca = AddInput(UiText.CaPath, previous.Ca);
        tls.IsCheckedChanged += (_, _) => UpdatePort();
        form.Children.Add(error);
        var submit = new Button
        {
            Content = UiText.Connect,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            Background = UiTheme.Primary
        };
        submit.Click += (_, _) => Submit();
        form.Children.Add(submit);
        Content = new ScrollViewer { Content = form };
    }

    private TextBox AddInput(string label, string text)
    {
        form.Children.Add(new TextBlock { Text = label, FontSize = UiTypography.Caption, Foreground = UiTheme.Muted });
        var box = new TextBox { Text = text };
        form.Children.Add(box);
        return box;
    }

    private void UpdatePort()
    {
        if (port.Text is ConnectionDefaults.PlainPortText or ConnectionDefaults.TlsPortText)
            port.Text = tls.IsChecked == true ? ConnectionDefaults.TlsPortText : ConnectionDefaults.PlainPortText;
    }

    private void Submit()
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
        catch (ArgumentException e)
        {
            error.Text = e.Message;
        }
    }
}
