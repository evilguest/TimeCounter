using Microsoft.Win32;
using System.Security.Cryptography;
using System.Text.Json;
namespace TimeCounter;
public sealed class Settings
{
    public int IdleMinutes { get; set; } = 5;
    public string FontName { get; set; } = "Agency FB";
    public float FontSize { get; set; } = 144;
    public int ColorArgb { get; set; } = Color.Red.ToArgb();
    public int Transparency { get; set; } = 75;
    public string Corner { get; set; } = "Справа сверху";
    public int OffsetX { get; set; } = 20;
    public int OffsetY { get; set; } = 20;
    public int Monitor { get; set; }
    public int Port { get; set; } = 47831;
    public string ApiToken { get; set; } = Convert.ToHexString(RandomNumberGenerator.GetBytes(24));
    public bool OverlayVisible { get; set; } = true;
    public static Settings Load()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\TimeCounter");
        var json = key?.GetValue("Settings") as string;
        var s = json == null ? new Settings() : JsonSerializer.Deserialize<Settings>(json) ?? new();
        if (s.IdleMinutes is < 1 or > 1440 || s.Port is < 1024 or > 65535 || s.FontSize is < 8 or > 500 || s.Transparency is < 0 or > 99 || s.ApiToken.Length < 16)
            throw new InvalidDataException("Некорректные настройки HKCU\\Software\\TimeCounter");
        return s;
    }
    public void Save()
    {
        using var key = Registry.CurrentUser.CreateSubKey(@"Software\TimeCounter");
        key.SetValue("Settings", JsonSerializer.Serialize(this));
    }
    public static bool AutoStart
    {
        get { using var k = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run"); return k?.GetValue("TimeCounter") != null; }
        set
        {
            using var k = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
            if (value) k.SetValue("TimeCounter", "\"" + Environment.ProcessPath + "\""); else k.DeleteValue("TimeCounter", false);
        }
    }
}
