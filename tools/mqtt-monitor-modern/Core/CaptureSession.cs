using System.Buffers;
using System.Net.Security;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading.Channels;
using MQTTnet;

namespace CakeAdaptMonitor;

public sealed record RawEvent(string Topic, double Received, byte[] Payload);

public sealed class CaptureSession
{
    private readonly BrokerSettings settings;
    private readonly IMqttClient client = new MqttClientFactory().CreateMqttClient();
    private readonly CancellationTokenSource stop = new();
    private readonly Channel<RawEvent> input = Channel.CreateBounded<RawEvent>(new BoundedChannelOptions(ConnectionDefaults.QueuedMessages)
    {
        SingleReader = true,
        FullMode = BoundedChannelFullMode.Wait
    });
    private Task? network;
    private Task? processor;
    private long received, processed, dropped;
    private volatile string status = ConnectionStatus.Disconnected;
    private volatile string error = "";
    private bool stopped;
    public MetricStore Store { get; } = new();
    public long Received => Interlocked.Read(ref received);
    public long Processed => Interlocked.Read(ref processed);
    public long Dropped => Interlocked.Read(ref dropped);
    public string Status => status;
    public string Error => error;
    public bool IsFinished => (network?.IsCompleted ?? true) && (processor?.IsCompleted ?? true);

    public CaptureSession(BrokerSettings settings)
    {
        this.settings = settings;
    }

    public void Start()
    {
        settings.Validate();
        var options = new MqttClientOptionsBuilder().WithTcpServer(settings.Host, settings.Port)
            .WithProtocolVersion(MQTTnet.Formatter.MqttProtocolVersion.V311)
            .WithCleanSession().WithKeepAlivePeriod(ConnectionDefaults.KeepAlive);
        if (settings.Username.Length != 0)
            options.WithCredentials(settings.Username, settings.Password);
        if (settings.Tls)
        {
            options.WithTlsOptions(tls =>
            {
                tls.UseTls().WithSslProtocols(SslProtocols.Tls12 | SslProtocols.Tls13);
                if (!string.IsNullOrWhiteSpace(settings.Ca))
                {
                    var roots = new X509Certificate2Collection();
                    roots.ImportFromPemFile(settings.Ca);
                    if (roots.Count == 0)
                        throw new ArgumentException("CA file contains no PEM certificates.");
                    tls.WithCertificateValidationHandler(args =>
                    {
                        // A private trust anchor never disables hostname validation.
                        if (args.Certificate == null ||
                            (args.SslPolicyErrors & (SslPolicyErrors.RemoteCertificateNameMismatch |
                             SslPolicyErrors.RemoteCertificateNotAvailable)) != 0)
                            return false;
                        using var chain = new X509Chain();
                        chain.ChainPolicy.TrustMode = X509ChainTrustMode.CustomRootTrust;
                        chain.ChainPolicy.CustomTrustStore.AddRange(roots);
                        chain.ChainPolicy.ApplicationPolicy.Add(new Oid(ConnectionDefaults.ServerAuthenticationOid));
                        chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
                        if (args.Chain != null)
                            foreach (var element in args.Chain.ChainElements)
                                chain.ChainPolicy.ExtraStore.Add(element.Certificate);
                        using var cert = X509CertificateLoader.LoadCertificate(args.Certificate.GetRawCertData());
                        return chain.Build(cert);
                    });
                }
            });
        }
        var builtOptions = options.Build();
        client.ApplicationMessageReceivedAsync += args =>
        {
            Interlocked.Increment(ref received);
            var message = args.ApplicationMessage;
            if (message.Payload.Length > ConnectionDefaults.PayloadBytes)
            {
                Interlocked.Increment(ref dropped);
                return Task.CompletedTask;
            }
            var item = new RawEvent(message.Topic, Telemetry.Now, message.Payload.ToArray());
            if (!input.Writer.TryWrite(item))
                Interlocked.Increment(ref dropped);
            return Task.CompletedTask;
        };
        processor = Task.Run(ProcessAsync);
        network = Task.Run(async () =>
        {
            try
            {
                while (!stop.IsCancellationRequested)
                {
                    try
                    {
                        if (!client.IsConnected)
                        {
                            status = ConnectionStatus.Connecting;
                            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
                            timeout.CancelAfter(ConnectionDefaults.ConnectTimeout);
                            await client.ConnectAsync(builtOptions, timeout.Token);
                            var subscription = new MqttClientFactory().CreateSubscribeOptionsBuilder()
                                .WithTopicFilter(settings.Topic).Build();
                            var result = await client.SubscribeAsync(subscription, timeout.Token);
                            if (result.Items.Any(item => (int)item.ResultCode >= ConnectionDefaults.SubscriptionFailure))
                                throw new IOException("Broker rejected the subscription.");
                            status = ConnectionStatus.Subscribed;
                            error = "";
                        }
                        await Task.Delay(ConnectionDefaults.NetworkInterval, stop.Token);
                    }
                    catch (Exception e) when (!stop.IsCancellationRequested)
                    {
                        status = ConnectionStatus.Retrying;
                        error = e.Message;
                        if (client.IsConnected)
                        {
                            using var timeout = new CancellationTokenSource(ConnectionDefaults.DisconnectTimeout);
                            try
                            {
                                await client.DisconnectAsync(new MqttClientDisconnectOptions(), timeout.Token);
                            }
                            catch (Exception) { /* Reconnect remains bounded by the outer loop. */ }
                        }
                        await Task.Delay(ConnectionDefaults.RetryInterval, stop.Token);
                    }
                }
            }
            catch (OperationCanceledException) { }
            catch (Exception) when (stop.IsCancellationRequested) { /* MQTTnet can wrap cancellation as connection failure. */ }
            finally
            {
                try
                {
                    if (client.IsConnected)
                    {
                        using var timeout = new CancellationTokenSource(ConnectionDefaults.DisconnectTimeout);
                        await client.DisconnectAsync(new MqttClientDisconnectOptions(), timeout.Token);
                    }
                }
                catch (Exception) { /* Transport disposal below still closes the socket. */ }
                finally
                {
                    client.Dispose();
                    input.Writer.TryComplete();
                }
            }
        });
    }

    private async Task ProcessAsync()
    {
        try
        {
            await foreach (var item in input.Reader.ReadAllAsync())
            {
                Store.Accept(item.Topic, Encoding.UTF8.GetString(item.Payload), item.Received);
                Interlocked.Increment(ref processed);
            }
        }
        catch (ArgumentException e)
        {
            error = "Telemetry processing stopped: " + e.Message;
            status = ConnectionStatus.DataError;
            stop.Cancel();
        }
    }

    public async Task StopAsync()
    {
        if (stopped)
            return;
        status = ConnectionStatus.Stopping;
        stop.Cancel();
        if (network != null)
            await network.WaitAsync(ConnectionDefaults.StopTimeout);
        else
            client.Dispose();
        input.Writer.TryComplete();
        if (processor != null)
            await processor.WaitAsync(ConnectionDefaults.StopTimeout);
        stop.Dispose();
        stopped = true;
        status = ConnectionStatus.Disconnected;
    }
}
