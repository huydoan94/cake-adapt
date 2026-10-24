using System.Buffers;
using System.Text;
using System.Threading.Channels;
using MQTTnet;

namespace CakeAdaptMonitor;

// Owns the client, cancellation and bounded handoff to the single decode worker.
public sealed class CaptureSession
{
    private sealed record RawMessage(string Topic, double Received, byte[] Payload);
    private readonly BrokerSettings settings;
    private readonly IMqttClient client = new MqttClientFactory().CreateMqttClient();
    private readonly CancellationTokenSource stop = new();
    private readonly Channel<RawMessage> input = Channel.CreateBounded<RawMessage>(new BoundedChannelOptions(ConnectionDefaults.QueuedMessages)
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
        var options = MqttOptions.Create(settings);
        client.ApplicationMessageReceivedAsync += args =>
        {
            Receive(args.ApplicationMessage);
            return Task.CompletedTask;
        };
        processor = Task.Run(ProcessAsync);
        network = Task.Run(() => RunNetworkAsync(options));
    }

    private void Receive(MqttApplicationMessage message)
    {
        Interlocked.Increment(ref received);
        if (message.Payload.Length > ConnectionDefaults.PayloadBytes)
        {
            Interlocked.Increment(ref dropped);
            return;
        }
        var item = new RawMessage(message.Topic, Telemetry.Now, message.Payload.ToArray());
        if (!input.Writer.TryWrite(item))
            Interlocked.Increment(ref dropped);
    }

    private async Task SubscribeAsync(MqttClientOptions options)
    {
        status = ConnectionStatus.Connecting;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
        timeout.CancelAfter(ConnectionDefaults.ConnectTimeout);
        await client.ConnectAsync(options, timeout.Token);
        var subscription = new MqttClientFactory().CreateSubscribeOptionsBuilder()
            .WithTopicFilter(settings.Topic).Build();
        var result = await client.SubscribeAsync(subscription, timeout.Token);
        if (result.Items.Any(item => (int)item.ResultCode >= ConnectionDefaults.SubscriptionFailure))
            throw new IOException("Broker rejected the subscription.");
        status = ConnectionStatus.Subscribed;
        error = "";
    }

    private async Task DisconnectClientAsync()
    {
        if (!client.IsConnected)
            return;
        using var timeout = new CancellationTokenSource(ConnectionDefaults.DisconnectTimeout);
        try
        {
            await client.DisconnectAsync(new MqttClientDisconnectOptions(), timeout.Token);
        }
        catch (Exception) { /* Retry or final disposal still closes the transport. */ }
    }

    private async Task RunNetworkAsync(MqttClientOptions options)
    {
        try
        {
            while (!stop.IsCancellationRequested)
            {
                try
                {
                    if (!client.IsConnected)
                        await SubscribeAsync(options);
                    await Task.Delay(ConnectionDefaults.NetworkInterval, stop.Token);
                }
                catch (Exception e) when (!stop.IsCancellationRequested)
                {
                    status = ConnectionStatus.Retrying;
                    error = e.Message;
                    await DisconnectClientAsync();
                    await Task.Delay(ConnectionDefaults.RetryInterval, stop.Token);
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception) when (stop.IsCancellationRequested) { /* MQTTnet may wrap cancellation. */ }
        finally
        {
            try
            {
                await DisconnectClientAsync();
            }
            finally
            {
                client.Dispose();
                input.Writer.TryComplete();
            }
        }
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
