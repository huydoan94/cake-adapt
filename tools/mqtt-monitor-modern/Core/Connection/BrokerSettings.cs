using System.Text;
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
        if (string.IsNullOrWhiteSpace(Host) || Host.Contains(ProtocolNames.UriSchemeMarker) ||
            Port < 1 || Port > ConnectionDefaults.MaximumPort)
            throw new ArgumentException("Enter a broker hostname without mqtt:// and a port from 1 to 65535.");
        ValidateTopic(Topic);
    }

    public static void ValidateTopic(string topic)
    {
        if (string.IsNullOrWhiteSpace(topic) || topic.Contains(ProtocolNames.InvalidCharacter) ||
            Encoding.UTF8.GetByteCount(topic) > ConnectionDefaults.MaximumTopicBytes)
            throw new ArgumentException("Enter a valid subscription topic.");
        var parts = topic.Split(ProtocolNames.TopicSeparator);
        for (var i = 0; i < parts.Length; i++)
            if ((parts[i].Contains(ProtocolNames.SingleWildcard) && parts[i] != ProtocolNames.SingleWildcard) ||
                (parts[i].Contains(ProtocolNames.MultiWildcard) && (parts[i] != ProtocolNames.MultiWildcard || i != parts.Length - 1)))
                throw new ArgumentException("Use + as a whole level and # as the final whole level.");
    }
}
