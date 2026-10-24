using System.Net.Security;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using MQTTnet;

namespace CakeAdaptMonitor;

internal static class MqttOptions
{
    public static MqttClientOptions Create(BrokerSettings settings)
    {
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
        return options.Build();
    }
}
