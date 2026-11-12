using TimeCounter;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;

string folder = Path.Combine(Path.GetTempPath(), "TimeCounterTests-" + Guid.NewGuid());
Directory.CreateDirectory(folder);
var now = new DateTimeOffset(2026, 11, 12, 12, 0, 0, TimeSpan.Zero);
void Equal(double expected, double actual, string label) { if (Math.Abs(expected - actual) > .001) throw new Exception($"{label}: expected {expected}, got {actual}"); }
var ledger = new Ledger(Path.Combine(folder, "history.json"));
var start = now.AddMinutes(-10);
Accounting.Record(ledger, start, now, true, start, TimeSpan.FromMinutes(5));
var day = ledger.History(0, now, TimeZoneInfo.Utc)[0];
Equal(300, day.ActiveSeconds, "idle deadline"); Equal(300, day.InactiveSeconds, "idle remainder");
Equal(43200, day.ActiveSeconds + day.InactiveSeconds + day.UncountedSeconds, "day partition");
ledger.Save(); var reload = new Ledger(Path.Combine(folder, "history.json")); Equal(300, reload.History(0, now, TimeZoneInfo.Utc)[0].ActiveSeconds, "persistence");
reload.Save(); if (!File.Exists(Path.Combine(folder, "history.json.bak"))) throw new Exception("backup missing");
var midnight = new Ledger(Path.Combine(folder, "midnight.json"));
midnight.Add(new DateTimeOffset(now.UtcDateTime.Date, TimeSpan.Zero).AddMinutes(-1), new DateTimeOffset(now.UtcDateTime.Date, TimeSpan.Zero).AddMinutes(1), true);
var rows = midnight.History(1, now, TimeZoneInfo.Utc);
Equal(60, rows[0].ActiveSeconds, "midnight today"); Equal(60, rows[1].ActiveSeconds, "midnight yesterday");
var overlap = new Ledger(Path.Combine(folder, "overlap.json"));
overlap.Add(now.AddMinutes(-2), now, true); overlap.Add(now.AddMinutes(-1), now.AddMinutes(1), true);
Equal(180, overlap.History(0, now.AddMinutes(1), TimeZoneInfo.Utc)[0].ActiveSeconds, "clock rollback overlap");
var disconnected = new Ledger(Path.Combine(folder, "disconnected.json"));
Accounting.Record(disconnected, start, now, false, now, TimeSpan.FromMinutes(5));
Equal(0, disconnected.History(0, now, TimeZoneInfo.Utc)[0].ActiveSeconds, "disconnected");
var zone = TimeZoneInfo.FindSystemTimeZoneById("Eastern Standard Time");
var dst = new Ledger(Path.Combine(folder, "dst.json"));
Equal(23 * 3600, dst.History(1, new DateTimeOffset(2026, 3, 9, 12, 0, 0, TimeSpan.Zero), zone)[1].ElapsedSeconds, "spring DST");
Equal(25 * 3600, dst.History(1, new DateTimeOffset(2026, 11, 2, 12, 0, 0, TimeSpan.Zero), zone)[1].ElapsedSeconds, "fall DST");
var probe = new TcpListener(IPAddress.Loopback, 0); probe.Start(); int port = ((IPEndPoint)probe.LocalEndpoint).Port; probe.Stop();
using var api = new ApiServer(port, "test-token", days => new { days = ledger.History(days, now, TimeZoneInfo.Utc) });
using var client = new HttpClient();
string url = $"http://127.0.0.1:{port}/api/history";
if ((await client.GetAsync(url)).StatusCode != HttpStatusCode.Unauthorized) throw new Exception("auth");
client.DefaultRequestHeaders.Authorization = new("Bearer", "test-token");
using var json = JsonDocument.Parse(await client.GetStringAsync(url));
if (json.RootElement.GetProperty("days").GetArrayLength() != 1) throw new Exception("default days");
using var previous = JsonDocument.Parse(await client.GetStringAsync(url + "?days=7"));
if (previous.RootElement.GetProperty("days").GetArrayLength() != 8) throw new Exception("previous days");
foreach (string query in new[] { "?days=-1", "?days=3661", "?days=x", "?x=1" })
 if ((await client.GetAsync(url + query)).StatusCode != HttpStatusCode.BadRequest) throw new Exception(query);
if ((await client.GetAsync($"http://127.0.0.1:{port}/other")).StatusCode != HttpStatusCode.NotFound) throw new Exception("route");
UiChecks.Run();
Console.WriteLine("PASS: UI and native window styles; idle, disconnect, midnight, DST, partition, persistence, clock rollback, HTTP authentication/routes/ranges");
Directory.Delete(folder, true);


