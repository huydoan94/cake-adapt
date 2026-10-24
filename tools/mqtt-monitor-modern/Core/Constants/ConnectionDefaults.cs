namespace CakeAdaptMonitor;

public static class ConnectionDefaults
{
    public const int TlsPort = 8883;
    public const string TlsPortText = "8883", PlainPortText = "1883";
    public const int PlainPort = 1883;
    public const int MaximumPort = 65535;
    public const int MaximumTopicBytes = 65535;
    public const string Topic = "cake-adapt/#";
    public const int QueuedMessages = 2048;
    public const int PayloadBytes = 65536;
    public const int SubscriptionFailure = 128;
    public const string ServerAuthenticationOid = "1.3.6.1.5.5.7.3.1";
    public static readonly TimeSpan KeepAlive = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);
    public static readonly TimeSpan DisconnectTimeout = TimeSpan.FromSeconds(2);
    public static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan NetworkInterval = TimeSpan.FromSeconds(1);
    public static readonly TimeSpan RetryInterval = TimeSpan.FromSeconds(3);
}
public static class ConnectionStatus
{
    public const string Disconnected = "Disconnected";
    public const string Connecting = "Connecting";
    public const string Subscribed = "Subscribed";
    public const string Retrying = "Retrying";
    public const string Stopping = "Stopping";
    public const string DataError = "Data error";
}
