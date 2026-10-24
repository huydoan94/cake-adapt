using System.Net;
using System.Net.Sockets;
using System.Net.Security;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using CakeAdaptMonitor;

static partial class Tests
{
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
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var server = Task.Run(async () =>
        {
            try
            {
                using var peer = await listener.AcceptTcpClientAsync(timeout.Token);
                await using var stream = new SslStream(peer.GetStream());
                await stream.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
                {
                    ServerCertificate = certificate,
                    EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
                }, timeout.Token);
                await Packet(stream, timeout.Token);
                await stream.WriteAsync(new byte[] { 0x20, 2, 0, 0 }, timeout.Token);
                var subscribe = await Packet(stream, timeout.Token);
                await stream.WriteAsync(new byte[] { 0x90, 3, subscribe[1], subscribe[2], 0 }, timeout.Token);
                await Packet(stream, timeout.Token); // Client disconnects after subscription.
            }
            catch (Exception e) when (mode != "valid" && e is AuthenticationException or IOException) { }
        });
        var session = new CaptureSession(new BrokerSettings
        {
            Host = "127.0.0.1",
            Port = ((IPEndPoint)listener.LocalEndpoint).Port,
            Tls = true,
            Ca = Path.Combine(directory, "root.pem")
        });
        try
        {
            session.Start();
            if (mode == "valid")
            {
                while (session.Status != "Subscribed")
                    await Task.Delay(20, timeout.Token);
                Check(session.Status == "Subscribed", "verified private-CA TLS accepts valid broker");
            }
            else
            {
                while (session.Error.Length == 0)
                    await Task.Delay(20, timeout.Token);
                Check(session.Status != "Subscribed" && session.Processed == 0, "TLS rejects " + mode + " certificate");
            }
            await session.StopAsync();
            await server;
        }
        finally { listener.Stop(); Directory.Delete(directory, true); }
    }


}
