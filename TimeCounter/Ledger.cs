using System.Text.Json;

namespace TimeCounter;

public sealed record Segment(DateTimeOffset Start, DateTimeOffset End, bool Active);
public sealed record DayRow(string Date, double ActiveSeconds, double InactiveSeconds, double UncountedSeconds, double ElapsedSeconds);

public sealed class Ledger
{
    readonly object gate = new();
    readonly string path;
    readonly List<Segment> segments;
    public Ledger(string path)
    {
        this.path = path;
        if (!File.Exists(path)) { segments = []; return; }
        try { segments = JsonSerializer.Deserialize<List<Segment>>(File.ReadAllText(path)) ?? throw new InvalidDataException("Empty history"); }
        catch (Exception ex) { throw new InvalidDataException("Не удалось прочитать историю. Файл сохранён без изменений: " + path, ex); }
    }
    public void Add(DateTimeOffset start, DateTimeOffset end, bool active)
    {
        if (end <= start) return;
        lock (gate)
        {
            // Never count the same wall-clock interval twice after a clock correction.
            if (segments.Count > 0 && start < segments[^1].End) start = segments[^1].End;
            if (end <= start) return;
            if (segments.Count > 0 && segments[^1].Active == active && segments[^1].End == start)
                segments[^1] = segments[^1] with { End = end };
            else segments.Add(new(start, end, active));
        }
    }
    public void Save()
    {
        lock (gate)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string temp = path + ".tmp";
            using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
            { JsonSerializer.Serialize(stream, segments); stream.Flush(true); }
            if (File.Exists(path)) File.Replace(temp, path, path + ".bak");
            else File.Move(temp, path);
        }
    }
    public List<DayRow> History(int days, DateTimeOffset now, TimeZoneInfo zone)
    {
        lock (gate)
        {
            var today = TimeZoneInfo.ConvertTime(now, zone).Date;
            List<DayRow> result = [];
            for (int i = 0; i <= days; i++)
            {
                var date = today.AddDays(-i);
                var begin = Boundary(date, zone);
                var finish = Boundary(date.AddDays(1), zone);
                if (finish > now) finish = now;
                double active = 0, inactive = 0;
                foreach (var s in segments)
                {
                    var a = s.Start > begin ? s.Start : begin;
                    var b = s.End < finish ? s.End : finish;
                    if (b <= a) continue;
                    if (s.Active) active += (b - a).TotalSeconds; else inactive += (b - a).TotalSeconds;
                }
                double elapsed = Math.Max(0, (finish - begin).TotalSeconds);
                result.Add(new(date.ToString("yyyy-MM-dd"), active, inactive, Math.Max(0, elapsed - active - inactive), elapsed));
            }
            return result;
        }
    }
    static DateTimeOffset Boundary(DateTime date, TimeZoneInfo zone)
    {
        date = DateTime.SpecifyKind(date, DateTimeKind.Unspecified);
        while (zone.IsInvalidTime(date)) date = date.AddMinutes(1);
        return new DateTimeOffset(TimeZoneInfo.ConvertTimeToUtc(date, zone), TimeSpan.Zero);
    }
}

public static class Accounting
{
    // Sample the previous state up to its idle deadline. Input after that deadline
    // starts a new active interval instead of filling the intervening idle gap.
    public static void Record(Ledger ledger, DateTimeOffset from, DateTimeOffset to,
        bool available, DateTimeOffset lastInput, TimeSpan timeout)
    {
        if (to <= from) return;
        if (!available) { ledger.Add(from, to, false); return; }
        var deadline = lastInput + timeout;
        var split = deadline < to ? deadline : to;
        if (split > from) ledger.Add(from, split, true);
        if (to > split) ledger.Add(split > from ? split : from, to, false);
    }
}

