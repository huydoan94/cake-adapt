using System.Text.Json.Serialization;
namespace CakeAdaptMonitor;

public sealed class BrokerSettings
{
    public string Host { get; set; } = "";
    public int Port { get; set; } = ConnectionDefaults.TlsPort;
    public string Topic { get; set; } = ConnectionDefaults.Topic;
    public string Username { get; set; } = "";
    [JsonIgnore] public string Password { get; set; } = "";
    public bool Tls { get; set; } = true;
    public string Ca { get; set; } = "";
    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Host) || Host.Contains(ProtocolNames.UriSchemeMarker) || (Port < 1 || Port > ConnectionDefaults.MaximumPort))
            throw new ArgumentException("Enter a broker hostname without mqtt:// and a port from 1 to 65535.");
        Telemetry.ValidateTopic(Topic);
    }
}
