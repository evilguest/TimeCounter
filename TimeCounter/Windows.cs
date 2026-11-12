using System.Runtime.InteropServices;
namespace TimeCounter;
internal static class Native
{
    [StructLayout(LayoutKind.Sequential)] struct LastInput { public uint Size, Time; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LastInput info);
    [DllImport("wtsapi32.dll", SetLastError = true)] internal static extern bool WTSRegisterSessionNotification(IntPtr hwnd, int flags);
    [DllImport("wtsapi32.dll")] internal static extern bool WTSUnRegisterSessionNotification(IntPtr hwnd);
    [DllImport("wtsapi32.dll")] static extern bool WTSQuerySessionInformation(IntPtr server, int session, int info, out IntPtr buffer, out int bytes);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(IntPtr memory);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    internal static DateTimeOffset InputTime(DateTimeOffset now)
    {
        var input = new LastInput { Size = (uint)Marshal.SizeOf<LastInput>() };
        if (!GetLastInputInfo(ref input)) throw new System.ComponentModel.Win32Exception();
        uint idle = unchecked((uint)Environment.TickCount64 - input.Time);
        return now.AddMilliseconds(-idle);
    }
    internal static bool SessionAvailable()
    {
        if (!WTSQuerySessionInformation(IntPtr.Zero, System.Diagnostics.Process.GetCurrentProcess().SessionId, 8, out var p, out _)) return false;
        int state; try { state = Marshal.ReadInt32(p); } finally { WTSFreeMemory(p); }
        if (state != 0) return false;
        var desktop = OpenInputDesktop(0, false, 0x0100);
        if (desktop == IntPtr.Zero) return false;
        CloseDesktop(desktop); return true;
    }
}
internal sealed class SessionWindow : Form
{
    public event Action<int>? SessionChanged;
    public event Action<bool>? PowerChanged;
    protected override void SetVisibleCore(bool value) => base.SetVisibleCore(false);
    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (!Native.WTSRegisterSessionNotification(Handle, 0)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    }
    protected override void WndProc(ref Message m)
    {
        if (m.Msg == 0x2B1) SessionChanged?.Invoke(m.WParam.ToInt32());
        if (m.Msg == 0x218)
        {
            if (m.WParam.ToInt32() == 4) PowerChanged?.Invoke(true);
            if (m.WParam.ToInt32() is 7 or 18) PowerChanged?.Invoke(false);
        }
        base.WndProc(ref m);
    }
    protected override void Dispose(bool disposing)
    { if (IsHandleCreated) Native.WTSUnRegisterSessionNotification(Handle); base.Dispose(disposing); }
}
internal sealed class Overlay : Form
{
    string value = "00:00:00";
    Settings settings = new();
    public Overlay()
    {
        FormBorderStyle = FormBorderStyle.None; ShowInTaskbar = false; TopMost = true;
        BackColor = Color.FromArgb(1, 2, 3); TransparencyKey = BackColor;
        DoubleBuffered = true; AutoScaleMode = AutoScaleMode.None;
    }
    protected override bool ShowWithoutActivation => true;
    protected override CreateParams CreateParams
    { get { var p = base.CreateParams; p.ExStyle |= 0x80000 | 0x20 | 0x08000000 | 0x80; return p; } }
    protected override void WndProc(ref Message m)
    { if (m.Msg == 0x84) { m.Result = new IntPtr(-1); return; } if (m.Msg == 0x21) { m.Result = new IntPtr(3); return; } base.WndProc(ref m); }
    public void UpdateDisplay(string text, Settings s)
    {
        value = text; settings = s; Opacity = (100 - s.Transparency) / 100.0;
        using var font = new Font(s.FontName, s.FontSize, FontStyle.Regular, GraphicsUnit.Point);
        var size = TextRenderer.MeasureText(value, font);
        var screens = Screen.AllScreens;
        var area = screens[Math.Clamp(s.Monitor, 0, screens.Length - 1)].WorkingArea;
        Size = new Size(Math.Min(area.Width, size.Width + 8), Math.Min(area.Height, size.Height + 8));
        int x = s.Corner.StartsWith("Справа") ? area.Right - Width - s.OffsetX : area.Left + s.OffsetX;
        int y = s.Corner.EndsWith("снизу") ? area.Bottom - Height - s.OffsetY : area.Top + s.OffsetY;
        Location = new Point(Math.Clamp(x, area.Left, Math.Max(area.Left, area.Right - Width)), Math.Clamp(y, area.Top, Math.Max(area.Top, area.Bottom - Height)));
        if (s.OverlayVisible && !Visible) Show(); else if (!s.OverlayVisible && Visible) Hide();
        TopMost = true; Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        using var font = new Font(settings.FontName, settings.FontSize, FontStyle.Regular, GraphicsUnit.Point);
        TextRenderer.DrawText(e.Graphics, value, font, ClientRectangle, Color.FromArgb(settings.ColorArgb), TextFormatFlags.NoPadding | TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }
}
