using System.Net;
using System.Net.Sockets;
using System.Text;
using CakeAdaptMonitor;

static partial class Tests
{
    private static async Task<byte[]> Packet(Stream stream, CancellationToken token)
    {
        var first = new byte[1];
        await stream.ReadExactlyAsync(first, token);
        var length = 0;
        var multiplier = 1;
        for (var i = 0; i < 4; i++)
        {
            var next = new byte[1];
            await stream.ReadExactlyAsync(next, token);
            length += (next[0] & 127) * multiplier;
            if ((next[0] & 128) == 0)
                break;
            multiplier *= 128;
        }
        if (length > 65536)
            throw new IOException("Oversize test packet");
        var payload = new byte[length + 1];
        payload[0] = first[0];
        await stream.ReadExactlyAsync(payload.AsMemory(1), token);
        return payload;
    }

    private static async Task Integration()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
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
            packet.AddRange(topic);
            packet.AddRange(body);
            await stream.WriteAsync(packet.ToArray(), timeout.Token);
            while (!timeout.IsCancellationRequested)
            {
                var next = await Packet(stream, timeout.Token);
                if ((next[0] >> 4) == 14)
                    break;
                if ((next[0] >> 4) == 12)
                    await stream.WriteAsync(new byte[] { 0xd0, 0 }, timeout.Token);
            }
        });
        var session = new CaptureSession(new BrokerSettings { Host = "127.0.0.1", Port = port, Tls = false });
        try
        {
            session.Start();
            while (session.Processed == 0 && !timeout.IsCancellationRequested)
                await Task.Delay(30, timeout.Token);
            await session.StopAsync();
            await broker;
            Check(session.Store.Snapshot(60).Any(m => m.Name == "RSS_KB" && m.Latest == "1000"), "broker message reaches metrics");
            Check(session.Processed == 1, "MQTT telemetry decoded in memory");
            Check(session.IsFinished, "network and decoder stop cleanly");
        }
        finally { listener.Stop(); }
    }

    private static async Task CancelPendingConnection()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var session = new CaptureSession(new BrokerSettings
        {
            Host = "127.0.0.1",
            Port = ((IPEndPoint)listener.LocalEndpoint).Port,
            Tls = false
        });
        try
        {
            var accepted = listener.AcceptTcpClientAsync(timeout.Token).AsTask();
            session.Start();
            using var peer = await accepted;
            await Packet(peer.GetStream(), timeout.Token); // Intentionally withhold CONNACK.
            await session.StopAsync();
            Check(session.IsFinished, "disconnect cancels pending MQTT handshake");
        }
        finally { listener.Stop(); }
    }


}
