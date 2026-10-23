using System.Net;
using System.Net.Sockets;
using System.Net.Security;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.VisualTree;
using Avalonia.Threading;
using CakeAdaptMonitor;

static partial class Tests
{
    private static int count;
    private static void Check(bool condition, string name)
    {
        if (!condition) throw new Exception("FAIL: " + name);
        Console.WriteLine("PASS: " + name); count++;
    }
    public static async Task Main(string[] args)
    {
        var store = new MetricStore { MaxSeries = 6 };
        store.Accept("cake-adapt/a", "LOAD; date; 100; 1; 1000; 2000; 3000; 4000", 100);
        Check(store.Snapshot(60, 100).Single(m => m.Name == "DL_ACHIEVED_RATE_KBPS").Latest == "1000", "current schema without startup header");
        store.Accept("cake-adapt/a", "FUTURE_HEADER; LOG_TIMESTAMP; NEW_US\nFUTURE; 100; 42", 100);
        Check(store.Snapshot(60, 100).Single(m => m.Name == "NEW_US").Latest == "42", "dynamic headers");
        var separate = new MetricStore();
        separate.Accept("a", "CUSTOM_HEADER; LOG_TIMESTAMP; VALUE", 100);
        separate.Accept("b", "CUSTOM; 100; 99", 100);
        Check(separate.SeriesCount == 0, "headers scoped to topic");
        separate.Accept("a", "CPU; date; 100; 1; 25; 10; 40", 100);
        Check(separate.Snapshot(60, 100).Single(m => m.Name == "CPU1_USAGE").Latest == "40", "dynamic CPU fallback");
        separate.Accept("a", "TCP_QUEUE; date; 100; 1; 0; 999; 1; 20", 100);
        Check(separate.Snapshot(60, 100).Single(m => m.Name == "DL_QUEUE_US").Samples[0].Value == null, "invalid queues produce gaps");
        for (var i = 0; i < 1000; i++) separate.Accept("header" + i, "X_HEADER; LOG_TIMESTAMP; VALUE", 100);
        Check(separate.HeaderCount <= 256 && separate.EvictedHeaders > 0, "header cache bounded");
        for (var i = 0; i < 1000; i++) separate.Accept("header999", "X_HEADER; LOG_TIMESTAMP; VALUE", 100);
        Check(separate.HeaderCount <= 256, "repeated headers do not grow cache");
        var bounded = new MetricStore { MaxSeries = 2 };
        for (var i = 0; i < 10; i++) bounded.Accept("a", $"X_HEADER; LOG_TIMESTAMP; VALUE\nX; {i}; {i}", i);
        Check(bounded.Snapshot(60, 9).Single().Samples.Length == 10, "available short history retained");
        Check(bounded.Snapshot(.5, 9).Single().Samples.Length == 1, "receive-time window");
        bounded.Accept("b", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 9; 8", 9);
        bounded.Accept("c", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 9; 8", 9);
        Check(bounded.SeriesCount == 2 && bounded.EvictedSeries == 1, "series count bounded");
        Check(Telemetry.Number("nan") == null && Telemetry.Number("1e999") == null, "nonfinite payloads excluded");
        Check(double.IsFinite(Telemetry.Mean([1.79e308, 1.79e308])) && Telemetry.Mean([-1e308, 1e308]) == 0, "finite extreme means");
        var invalid = new MetricStore();
        invalid.Accept("a", "X_HEADER; LOG_TIMESTAMP; VALUE\nX; 100; 1\nX; 101; NaN\nX; 102; 3", 100);
        Check(invalid.Snapshot(60, 100).Single().Samples[1].Value == null, "invalid numeric updates create chart gaps");
        var key = new MetricKey("a", "SUMMARY", "", "DL_ACHIEVED_RATE_KBPS");
        var stale = new MetricView(key, "1", 1, []);
        var fresh = new MetricView(key with { Record = "LOAD" }, "5", 100, [new Sample(100, 100, 5)]);
        Check(Telemetry.SelectSeries([stale, fresh]).Single() == fresh, "live LOAD replaces stale SUMMARY");
        var settings = new BrokerSettings { Host = "example", Username = "user", Password = "secret" };
        Check(!JsonSerializer.Serialize(settings).Contains("secret"), "password never serialized");
        foreach (var topic in new[] { "#", "cake-adapt/#", "cake-adapt/+/main" }) Telemetry.ValidateTopic(topic);
        foreach (var topic in new[] { "", "a#", "a/#/b", "a/+b" })
        {
            var rejected = false;
            try { Telemetry.ValidateTopic(topic); } catch (ArgumentException) { rejected = true; }
            Check(rejected, "reject invalid subscription " + topic);
        }
        var hourly = new MetricStore();
        hourly.Accept("history", "X_HEADER; LOG_TIMESTAMP; VALUE", 0);
        for (var i = 0; i <= 18000; i++)
        {
            var time = i / 5.0;
            hourly.Accept("history", $"X; {time:R}; {(i == 7 ? 9999 : i % 10)}", time);
        }
        var hourPoints = hourly.Snapshot(3600, 3600).Single().Samples;
        Check(hourPoints.Min(p => p.ReceivedTime) <= 1 && hourPoints.Max(p => p.ReceivedTime) == 3600, "five-Hz input retains the full hour");
        Check(hourPoints.Any(p => p.Value == 9999), "downsampling preserves an old isolated spike");
        var fiveMinutes = hourly.Snapshot(300, 3600).Single().Samples;
        Check(fiveMinutes.Min(p => p.ReceivedTime) <= 3302 && fiveMinutes.Max(p => p.ReceivedTime) == 3600, "five-minute window no longer cuts off at 600 readings");
        var history = new MetricHistory();
        for (var i = 0; i <= 72000; i++) history.Add(new Sample(i / 20.0, i / 20.0, i % 50));
        Check(history.BucketCount <= 610 && history.Read(3600, 3600).Length <= 3050, "high-rate history has bounded buckets and render points");
        Check(history.Read(3600, 8000).Length == 0, "expired history is removed by time");
        await Integration();
        await CancelPendingConnection();
        await TlsIntegration("valid");
        await TlsIntegration("wrong-name");
        await TlsIntegration("client-only");
        CoreRefactorChecks();
        RatePeakChecks();
        Preview(args.Length > 0 ? args[0] : "preview.png");
        Console.WriteLine($"{count} checks passed");
    }

    private static async Task<byte[]> Packet(Stream stream, CancellationToken token)
    {
        var first = new byte[1]; await stream.ReadExactlyAsync(first, token);
        var length = 0; var multiplier = 1;
        for (var i = 0; i < 4; i++)
        {
            var next = new byte[1]; await stream.ReadExactlyAsync(next, token);
            length += (next[0] & 127) * multiplier;
            if ((next[0] & 128) == 0) break;
            multiplier *= 128;
        }
        if (length > 65536) throw new IOException("Oversize test packet");
        var payload = new byte[length + 1]; payload[0] = first[0];
        await stream.ReadExactlyAsync(payload.AsMemory(1), token); return payload;
    }

    private static async Task Integration()
    {
        var directory = Path.Combine(Path.GetTempPath(), "cake-monitor-test-" + Guid.NewGuid());
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var broker = Task.Run(async () =>
        {
            using var peer = await listener.AcceptTcpClientAsync(timeout.Token);
            await using var stream = peer.GetStream();
            var connect = await Packet(stream, timeout.Token);
            Check((connect[0] >> 4) == 1, "real MQTT CONNECT");
            await stream.WriteAsync(new byte[] { 0x20, 2, 0, 0 }, timeout.Token);
            var subscribe = await Packet(stream, timeout.Token);
            Check((subscribe[0] >> 4) == 8, "real MQTT SUBSCRIBE");
            await stream.WriteAsync(new byte[] { 0x90, 3, subscribe[1], subscribe[2], 0 }, timeout.Token);
            var topic = Encoding.UTF8.GetBytes("cake-adapt/test");
            var body = Encoding.UTF8.GetBytes("MEMORY; date; 100; 1; 1000; 1200; 700; 1500");
            var packet = new List<byte> { 0x30, (byte)(2 + topic.Length + body.Length), 0, (byte)topic.Length };
            packet.AddRange(topic); packet.AddRange(body);
            await stream.WriteAsync(packet.ToArray(), timeout.Token);
            while (!timeout.IsCancellationRequested)
            {
                var next = await Packet(stream, timeout.Token);
                if ((next[0] >> 4) == 14) break;
                if ((next[0] >> 4) == 12) await stream.WriteAsync(new byte[] { 0xd0, 0 }, timeout.Token);
            }
        });
        var session = new CaptureSession(new BrokerSettings { Host = "127.0.0.1", Port = port, Tls = false });
        try
        {
            session.Start();
            while (session.Processed == 0 && !timeout.IsCancellationRequested) await Task.Delay(30, timeout.Token);
            await session.StopAsync();
            await broker;
            Check(session.Store.Snapshot(60).Any(m => m.Name == "RSS_KB" && m.Latest == "1000"), "broker message reaches metrics");
            Check(session.Processed == 1, "MQTT telemetry decoded in memory");
            Check(session.IsFinished, "network and decoder stop cleanly");
        }
        finally { listener.Stop();  }
    }

    private static async Task TlsIntegration(string mode)
    {
        var directory = Path.Combine(Path.GetTempPath(), "cake-monitor-tls-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        using var rootKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var rootRequest = new CertificateRequest("CN=Local monitor test root", rootKey, HashAlgorithmName.SHA256);
        rootRequest.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
        rootRequest.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.KeyCertSign, true));
        using var root = rootRequest.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        File.WriteAllText(Path.Combine(directory, "root.pem"), root.ExportCertificatePem());
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var request = new CertificateRequest("CN=Local test broker", key, HashAlgorithmName.SHA256);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
        var san = new SubjectAlternativeNameBuilder();
        san.AddIpAddress(mode == "wrong-name" ? IPAddress.Parse("192.0.2.1") : IPAddress.Loopback);
        request.CertificateExtensions.Add(san.Build());
        request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection {
            new Oid(mode == "client-only" ? "1.3.6.1.5.5.7.3.2" : "1.3.6.1.5.5.7.3.1") }, true));
        using var issued = request.Create(root, DateTimeOffset.UtcNow.AddHours(-1), DateTimeOffset.UtcNow.AddHours(1), RandomNumberGenerator.GetBytes(16));
        using var certificate = issued.CopyWithPrivateKey(key);
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var server = Task.Run(async () =>
        {
            try
            {
                using var peer = await listener.AcceptTcpClientAsync(timeout.Token);
                await using var stream = new SslStream(peer.GetStream());
                await stream.AuthenticateAsServerAsync(new SslServerAuthenticationOptions { ServerCertificate = certificate,
                    EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13 }, timeout.Token);
                await Packet(stream, timeout.Token);
                await stream.WriteAsync(new byte[] { 0x20, 2, 0, 0 }, timeout.Token);
                var subscribe = await Packet(stream, timeout.Token);
                await stream.WriteAsync(new byte[] { 0x90, 3, subscribe[1], subscribe[2], 0 }, timeout.Token);
                await Packet(stream, timeout.Token); // Client disconnects after subscription.
            }
            catch (Exception e) when (mode != "valid" && e is AuthenticationException or IOException) { }
        });
        var session = new CaptureSession(new BrokerSettings { Host = "127.0.0.1", Port = ((IPEndPoint)listener.LocalEndpoint).Port,
            Tls = true, Ca = Path.Combine(directory, "root.pem") });
        try
        {
            session.Start();
            if (mode == "valid")
            {
                while (session.Status != "Subscribed") await Task.Delay(20, timeout.Token);
                Check(session.Status == "Subscribed", "verified private-CA TLS accepts valid broker");
            }
            else
            {
                while (session.Error.Length == 0) await Task.Delay(20, timeout.Token);
                Check(session.Status != "Subscribed" && session.Processed == 0, "TLS rejects " + mode + " certificate");
            }
            await session.StopAsync();
            await server;
        }
        finally { listener.Stop(); Directory.Delete(directory, true); }
    }

    private static async Task CancelPendingConnection()
    {
        var directory = Path.Combine(Path.GetTempPath(), "cake-monitor-cancel-" + Guid.NewGuid());
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var session = new CaptureSession(new BrokerSettings { Host = "127.0.0.1",
            Port = ((IPEndPoint)listener.LocalEndpoint).Port, Tls = false });
        try
        {
            var accepted = listener.AcceptTcpClientAsync(timeout.Token).AsTask();
            session.Start();
            using var peer = await accepted;
            await Packet(peer.GetStream(), timeout.Token); // Intentionally withhold CONNACK.
            await session.StopAsync();
            Check(session.IsFinished, "disconnect cancels pending MQTT handshake");
        }
        finally { listener.Stop();  }
    }

    private static void Click(Window window, string label) => window.GetVisualDescendants().OfType<Button>()
        .Single(b => Equals(b.Content, label)).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
    private static void Pump() { Dispatcher.UIThread.RunJobs(); AvaloniaHeadlessPlatform.ForceRenderTimerTick(); }
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
        using (var bitmap = dialog.CaptureRenderedFrame()) bitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "settings-preview.png"));
        var choices = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(choices.Length == 4, "chart settings exposes current throughput traces");
        foreach (var choice in choices) choice.IsChecked = ((TraceKey)choice.Tag!).Name == "DL_ACHIEVED_RATE_KBPS";
        Click(dialog, "Apply"); Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Label.StartsWith("DL traffic"), "applying settings draws only the chosen trace");
        var now = Telemetry.Now;
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {now:R}; 1; 54321; 5000; 180000; 25000", now);
        window.Refresh(); Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Points.Any(p => p.Value == 54.321), "selection survives updates and record changes");
        panel.Select("CPU"); window.Refresh(); Pump();
        panel.Select("Throughput"); window.Refresh(); Pump();
        Check(chart.Lines.Length == 1, "trace choices are remembered per preset in memory");
        dialog = Open(); Click(dialog, "Clear"); Click(dialog, "Apply"); Pump();
        Check(chart.Lines.Length == 0 && chart.EmptyMessage.Contains("selected"), "clearing traces gives an explicit empty selection");
        dialog = Open(); Click(dialog, "Use defaults"); Pump();
        Check(chart.Lines.Length == 4, "defaults restore automatic traces");
        store.Accept("cake-adapt/sample/second", $"LOAD; date; {now:R}; 1; 30000; 5000; 150000; 25000", now);
        window.Refresh(); Pump();
        dialog = Open(); Click(dialog, "Select all"); Click(dialog, "Apply"); Pump();
        Check(chart.Lines.Length == 8, "settings can explicitly draw more than the automatic six traces");
        dialog = Open(); Click(dialog, "Clear"); dialog.Close(); Pump();
        Check(chart.Lines.Length == 8, "closing settings without applying preserves the plot");
        var source = window.GetVisualDescendants().OfType<ComboBox>()
            .Single(c => c.Items.OfType<string>().Contains("All sources"));
        source.SelectedItem = "cake-adapt/sample/main"; window.Refresh(); Pump();
        dialog = Open();
        choices = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(choices.Length == 4 && choices.All(c => ((TraceKey)c.Tag!).Topic == "cake-adapt/sample/main"),
            "settings only lists the selected topic");
        Check(choices.All(c => c.IsChecked == true), "a topic starts with its own automatic choices");
        foreach (var choice in choices) choice.IsChecked = ((TraceKey)choice.Tag!).Name == "DL_ACHIEVED_RATE_KBPS";
        Click(dialog, "Apply"); Pump();
        source.SelectedItem = "cake-adapt/sample/second"; window.Refresh(); Pump();
        Check(chart.Lines.Length == 4, "switching topics does not reuse another topic selection");
        dialog = Open();
        foreach (var choice in dialog.GetVisualDescendants().OfType<CheckBox>())
            choice.IsChecked = ((TraceKey)choice.Tag!).Name == "UL_ACHIEVED_RATE_KBPS";
        Click(dialog, "Apply"); Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Label.StartsWith("UL traffic"), "second topic can have an independent selection");
        source.SelectedItem = "cake-adapt/sample/main"; window.Refresh(); Pump();
        Check(chart.Lines.Length == 1 && chart.Lines[0].Label.StartsWith("DL traffic"), "switching back restores the topic selection");
        store.Accept("cake-adapt/sample/main", "COPY_HEADER; LOG_TIMESTAMP; REFLECTOR; CAKE_DL_RATE_KBPS\n" +
            $"COPY; {now:R}; sample-reflector; 180000", now);
        window.Refresh(); Pump();
        dialog = Open();
        choices = dialog.GetVisualDescendants().OfType<CheckBox>().ToArray();
        Check(choices.Length == 4 && choices.All(c => ((TraceKey)c.Tag!).Dimension.Length == 0),
            "global fields do not repeat for every reflector");
        Check(choices.Count(c => c.IsChecked == true) == 1, "settings reflects the restored topic selection");
        dialog.Width = 460; dialog.Height = 350; Pump();
        var apply = dialog.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Apply"));
        var position = apply.TranslatePoint(default, dialog);
        Check(position.HasValue && position.Value.Y >= 0 && position.Value.Y + apply.Bounds.Height <= dialog.ClientSize.Height,
            "settings actions remain visible at minimum window size");
        using (var bitmap = dialog.CaptureRenderedFrame()) bitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "settings-compact-preview.png"));
        dialog.Close(); Pump();
        source.SelectedItem = "All sources"; window.Refresh(); Pump();
        Check(chart.Lines.Length == 8, "All sources retains its separate selection");
        dialog = Open(); Click(dialog, "Use defaults"); Pump();
    }

    private static void Preview(string path)
    {
        AppBuilder.Configure<App>().UseSkia().WithInterFont()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false }).SetupWithoutStarting();
        var store = new MetricStore();
        var now = Telemetry.Now;
        for (var i = 0; i < 300; i++)
        {
            var t = now - 299 + i;
            var dl = 80000 + 35000 * Math.Sin(i / 20.0) + 25000 * Math.Sin(i / 6.0);
            store.Accept("cake-adapt/sample/main", $"LOAD; date; {t:R}; 100; {dl:F0}; {5000 + i * 20}; 180000; 25000", t);
            store.Accept("cake-adapt/sample/main", $"MEMORY; date; {t:R}; 100; {18000 + i * 2}; 19000; 10000; 30000", t);
            store.Accept("cake-adapt/sample/main", $"CPU; date; {t:R}; {t:R}; {30 + 15 * Math.Sin(i / 17.0):F0}; {20 + 10 * Math.Sin(i / 11.0):F0}; {40 + 25 * Math.Sin(i / 7.0):F0}", t);
            store.Accept("cake-adapt/sample/main", $"SUMMARY; date; {t:R}; {dl:F0}; {5000 + i * 20}; 1; 1; {2000 + 1200 * Math.Sin(i / 12.0):F0}; {1000 + 500 * Math.Sin(i / 8.0):F0}; low; low; 180000; 25000", t);
        }
        var window = new MainWindow { Width = 1440, Height = 920 };
        window.Preview(store); window.Show();
        Dispatcher.UIThread.RunJobs();
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var bitmap = window.CaptureRenderedFrame();
        if (bitmap == null) throw new Exception("Preview capture failed");
        bitmap.Save(path); Check(bitmap.PixelSize.Width > 1000, "dark dashboard rendered");
        Check(Application.Current!.RequestedThemeVariant == ThemeVariant.Dark, "dark theme enforced");
        Check(!window.GetVisualDescendants().OfType<Button>().Any(b => Equals(b.Content, "Export CSV")), "viewer has no CSV export control");
        store.PublishRatePeaks();
        var received = Telemetry.Now;
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {received:R}; 100; 85000; 6500; 180000; 25000", received);
        store.Accept("cake-adapt/sample/main", $"LOAD; date; {received:R}; 100; 40; 30; 180000; 25000", received);
        store.PublishRatePeaks(); window.Refresh(); Pump();
        string CardValue(string label)
        {
            var heading = window.GetVisualDescendants().OfType<TextBlock>().Single(t => t.Text == label);
            return ((StackPanel)heading.Parent!).Children.OfType<TextBlock>().ElementAt(1).Text!;
        }
        Check(CardValue("DOWNLOAD") == "85 Mbit/s" && CardValue("UPLOAD") == "6.5 Mbit/s",
            "Download and Upload labels stay unchanged while cards display interval peaks");
        Check(CardValue("CAKE DOWNLOAD") == "180 Mbit/s" && CardValue("CAKE UPLOAD") == "25 Mbit/s",
            "CAKE cards continue displaying configured rates");
        ChartSettingsFlow(window, store, path);
        var chart = window.GetVisualDescendants().OfType<TrendChart>().First();
        var axisEnd = chart.EndTime;
        Thread.Sleep(20); window.Refresh();
        Check(chart.EndTime > axisEnd, "chart time axis advances without new data");
        var navigate = window.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "All metrics"));
        navigate.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Dispatcher.UIThread.RunJobs();
        var table = window.GetVisualDescendants().OfType<DataGrid>().Single();
        Check(table.ItemsSource.Cast<MetricRow>().Any(), "metric navigation shows received metrics");
        var columnNames = Enumerable.Range(0, 140).Select(i => $"SCROLL_{i:000}").ToArray();
        store.Accept("cake-adapt/sample/main", "SCROLL_HEADER; LOG_TIMESTAMP; " + string.Join("; ", columnNames) +
            $"\nSCROLL; {Telemetry.Now:R}; " + string.Join("; ", Enumerable.Repeat("42", 140)), Telemetry.Now);
        window.Refresh(); Dispatcher.UIThread.RunJobs(); AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        var selectedRow = table.ItemsSource.Cast<MetricRow>().Single(r => r.Name == "SCROLL_100");
        table.SelectedItem = selectedRow;
        table.ScrollIntoView(selectedRow, table.Columns.Last());
        Dispatcher.UIThread.RunJobs(); AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        var vertical = table.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.ScrollBar>()
            .Single(b => b.Orientation == Avalonia.Layout.Orientation.Vertical);
        var horizontal = table.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.ScrollBar>()
            .Single(b => b.Orientation == Avalonia.Layout.Orientation.Horizontal);
        var beforeVertical = vertical.Value; var beforeHorizontal = horizontal.Value;
        Check(beforeVertical > 0 && beforeHorizontal > 0, "scroll regression exercises both axes");
        var originalSource = table.ItemsSource;
        store.Accept("cake-adapt/sample/main", $"SCROLL; {Telemetry.Now:R}; " + string.Join("; ", Enumerable.Repeat("43", 140)), Telemetry.Now);
        for (var i = 0; i < 3; i++)
        {
            window.Refresh(); Dispatcher.UIThread.RunJobs(); AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        }
        Check(ReferenceEquals(originalSource, table.ItemsSource) && ReferenceEquals(selectedRow, table.SelectedItem), "refresh preserves collection and selected row");
        Check(Math.Abs(vertical.Value - beforeVertical) < .01 && Math.Abs(horizontal.Value - beforeHorizontal) < .01, "refresh preserves vertical and horizontal scroll");
        Check(selectedRow.Latest == "43", "stable rows still receive new readings");
        var search = window.GetVisualDescendants().OfType<TextBox>().Single(t => t.Watermark?.ToString()?.StartsWith("Find a metric") == true);
        search.Text = "RSS_KB";
        Dispatcher.UIThread.RunJobs();
        Check(table.ItemsSource.Cast<MetricRow>().All(m => m.Name.Contains("RSS_KB")), "metric search filters table");
        AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var metricBitmap = window.CaptureRenderedFrame();
        metricBitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "metrics-preview.png"));
        var eventsNav = window.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Events"));
        eventsNav.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Dispatcher.UIThread.RunJobs();
        Check(window.GetVisualDescendants().OfType<TextBox>().Any(t => t.IsReadOnly && (t.Text?.Contains("cake-adapt/sample/main") ?? false)), "event view contains raw telemetry");
        var dialog = new ConnectionDialog(new BrokerSettings());
        dialog.Show(window); Dispatcher.UIThread.RunJobs(); AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var dialogBitmap = dialog.CaptureRenderedFrame();
        dialogBitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "connection-preview.png"));
        Check(dialog.GetVisualDescendants().OfType<CheckBox>().Single().IsChecked == true, "TLS enabled in connection dialog");
        dialog.Close();
        var bridgeTime = Telemetry.Now;
        var bridgeChart = new TrendChart { Seconds = 300, EndTime = bridgeTime, Lines = [new PlotLine("Bridge test",
            [new Sample(bridgeTime - 240, bridgeTime, 50), new Sample(bridgeTime - 150, bridgeTime, null),
             new Sample(bridgeTime - 60, bridgeTime, 50)], TrendChart.Palette[0])] };
        var bridgeWindow = new Window { Width = 500, Height = 300, Background = Brushes.Black, Content = bridgeChart };
        bridgeWindow.Show(window); Dispatcher.UIThread.RunJobs(); AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        using var bridgeBitmap = bridgeWindow.CaptureRenderedFrame();
        var pointer = System.Runtime.InteropServices.Marshal.AllocHGlobal(36);
        try
        {
            var x = (int)(64 + (bridgeChart.Bounds.Width - 82) / 2);
            bridgeBitmap!.CopyPixels(new PixelRect(x - 1, 14, 3, 3), pointer, 36, 12);
            var pixels = new byte[36]; System.Runtime.InteropServices.Marshal.Copy(pointer, pixels, 0, pixels.Length);
            var rgba = bridgeBitmap.Format == Avalonia.Platform.PixelFormat.Rgba8888;
            var blueLine = Enumerable.Range(0, 9).Any(i => pixels[i * 4 + (rgba ? 2 : 0)] > 160 &&
                pixels[i * 4 + (rgba ? 2 : 0)] - pixels[i * 4 + (rgba ? 0 : 2)] > 40);
            Check(blueLine, "rendered line bridges a long gap and invalid reading");
        }
        finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(pointer); }
        bridgeBitmap!.Save(Path.Combine(Path.GetDirectoryName(path)!, "bridge-preview.png"));
        bridgeWindow.Close();
        window.Close();
    }
}
