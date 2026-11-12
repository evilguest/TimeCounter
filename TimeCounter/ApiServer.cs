using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
namespace TimeCounter;
internal sealed class ApiServer : IDisposable
{
    readonly TcpListener listener;
    readonly CancellationTokenSource stop = new();
    readonly SemaphoreSlim clients = new(16);
    readonly Func<int, object> history;
    readonly string token;
    public ApiServer(int port, string token, Func<int, object> history)
    {
        this.history = history; this.token = token;
        listener = new TcpListener(IPAddress.IPv6Any, port);
        listener.Server.DualMode = true;
        listener.Start(16);
        _ = Accept();
    }
    async Task Accept()
    {
        try
        {
            while (!stop.IsCancellationRequested)
            {
                await clients.WaitAsync(stop.Token);
                TcpClient client;
                try { client = await listener.AcceptTcpClientAsync(stop.Token); }
                catch { clients.Release(); throw; }
                _ = Serve(client);
            }
        }
        catch (OperationCanceledException) { }
        catch (ObjectDisposedException) { }
        catch (Exception ex) { AppLog.Write(ex); }
    }
    async Task Serve(TcpClient client)
    {
        using (client)
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(stop.Token))
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(5));
            try
            {
                var stream = client.GetStream();
                byte[] buffer = new byte[8192]; int count = 0;
                while (count < buffer.Length)
                {
                    int n = await stream.ReadAsync(buffer.AsMemory(count, 1), timeout.Token);
                    if (n == 0) return;
                    count += n;
                    if (count >= 4 && Encoding.ASCII.GetString(buffer, count - 4, 4) == "\r\n\r\n") break;
                }
                var lines = Encoding.ASCII.GetString(buffer, 0, count).Split("\r\n");
                var request = lines[0].Split(' ');
                int status = 200; object body;
                string? auth = lines.FirstOrDefault(l => l.StartsWith("Authorization:", StringComparison.OrdinalIgnoreCase))?.Split(':', 2)[1].Trim();
                if (count == buffer.Length) { status = 431; body = new { error = "Headers too large" }; }
                else if (!CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(auth ?? ""), Encoding.UTF8.GetBytes("Bearer " + token)))
                { status = 401; body = new { error = "Bearer token required" }; }
                else if (request.Length != 3 || request[0] != "GET") { status = 405; body = new { error = "Use GET" }; }
                else if (!Uri.TryCreate("http://localhost" + request[1], UriKind.Absolute, out var uri) || uri.AbsolutePath != "/api/history")
                { status = 404; body = new { error = "Use /api/history?days=0" }; }
                else
                {
                    int days = 0; bool valid = true;
                    if (uri.Query.Length > 1)
                    {
                        var parts = uri.Query[1..].Split('=');
                        valid = parts.Length == 2 && parts[0] == "days" && int.TryParse(parts[1], out days) && days is >= 0 and <= 3660;
                    }
                    if (!valid) { status = 400; body = new { error = "days must be 0..3660 (previous days, plus today)" }; }
                    else body = history(days);
                }
                var payload = JsonSerializer.SerializeToUtf8Bytes(body, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
                var header = Encoding.ASCII.GetBytes($"HTTP/1.1 {status} Result\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {payload.Length}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
                await stream.WriteAsync(header, timeout.Token); await stream.WriteAsync(payload, timeout.Token);
            }
            catch (Exception ex) when (ex is IOException or OperationCanceledException or SocketException) { }
            catch (Exception ex) { AppLog.Write(ex); }
            finally { clients.Release(); }
        }
    }
    public void Dispose() { stop.Cancel(); listener.Stop(); }
}
internal static class AppLog
{
    internal static readonly string DirectoryPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TimeCounter");
    internal static void Write(Exception ex)
    { try { Directory.CreateDirectory(DirectoryPath); File.AppendAllText(Path.Combine(DirectoryPath, "errors.log"), DateTimeOffset.Now + " " + ex + Environment.NewLine); } catch { } }
}
