using Avalonia.Controls;
namespace CakeAdaptMonitor;

public sealed partial class MainWindow
{
    private void AttachConnectionLifecycle()
    {
        connect.Click += async (_, _) =>
        {
            if (busy)
                return;
            if (session != null)
            {
                await Disconnect();
                return;
            }
            var dialog = new ConnectionDialog(lastSettings);
            var settings = await dialog.ShowDialog<BrokerSettings?>(this);
            if (settings == null)
                return;
            try
            {
                lastSettings = settings;
                session = new CaptureSession(settings);
                session.Start();
                store = session.Store;
                selectedMetric = null;
                subtitle.Text = string.Format(UiText.SubtitleFormat, settings.Topic);
                notice.Text = "";
                connect.Content = UiText.Disconnect;
            }
            catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
            {
                notice.Text = e.Message;
                if (session != null)
                    await Disconnect();
            }
            Refresh();
        };
        Closing += async (_, e) =>
        {
            if (closingAllowed || session == null)
                return;
            e.Cancel = true;
            if (closeAttempted && !busy)
            {
                closingAllowed = true;
                Close();
                return;
            }
            closeAttempted = true;
            await Disconnect();
            if (session == null)
            {
                closingAllowed = true;
                Close();
            }
            else
                notice.Text = UiText.StillClosing;
        };
    }
    private async Task Disconnect()
    {
        if (session == null || busy)
            return;
        busy = true;
        connect.IsEnabled = false;
        try
        {
            await session.StopAsync();
            var lastError = session.Error;
            session = null;
            status.Text = ConnectionStatus.Disconnected;
            connect.Content = UiText.ConnectBroker;
            notice.Text = lastError.Length == 0 ? UiText.DisconnectedNotice : UiText.CaptureStopped + lastError;
        }
        catch (Exception e) when (e is TimeoutException or IOException or OperationCanceledException)
        {
            notice.Text = UiText.StopPending + e.Message;
        }
        finally { busy = false; connect.IsEnabled = true; }
    }
}
