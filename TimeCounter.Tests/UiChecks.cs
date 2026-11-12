using System.Runtime.InteropServices;
using TimeCounter;
internal static class UiChecks
{
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool GetMenuItemRect(IntPtr owner, IntPtr menu, uint item, out Rect rect);
    static void Pump(int milliseconds)
    {
        var watch = System.Diagnostics.Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < milliseconds) { Application.DoEvents(); Thread.Sleep(10); }
    }
    static T Field<T>(CounterContext context, string name) => (T)typeof(CounterContext).GetField(name, System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.GetValue(context)!;
    static void CheckTray()
    {
        string folder = Path.Combine(Path.GetTempPath(), "TimeCounterTray-" + Guid.NewGuid());
        var probe = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
        probe.Start(); int port = ((System.Net.IPEndPoint)probe.LocalEndpoint).Port; probe.Stop();
        using var context = new CounterContext(new Settings { Port = port }, new Ledger(Path.Combine(folder, "history.json")), _ => { });
        var overlay = Field<Overlay>(context, "overlay");
        var events = Field<SessionWindow>(context, "events");
        var cursor = Cursor.Position;
        void Click(int index)
        {
            SetCursorPos(200, 200);
            var click = Task.Run(() =>
            {
                Thread.Sleep(1200); // Cross a real accounting tick inside the native menu loop.
                var menu = Field<IntPtr>(context, "activeMenu");
                if (menu == IntPtr.Zero || !GetMenuItemRect(IntPtr.Zero, menu, (uint)index, out var bounds))
                {
                    Native.PostMessageW(events.Handle, 0x1F, UIntPtr.Zero, IntPtr.Zero);
                    throw new Exception("tray menu closed during timer tick");
                }
                SetCursorPos(bounds.Left + 20, (bounds.Top + bounds.Bottom) / 2);
                Thread.Sleep(50);
                mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero);
            });
            context.ShowTrayMenu();
            click.GetAwaiter().GetResult();
            Pump(1200);
        }
        try
        {
            Click(0); if (IsWindowVisible(overlay.Handle)) throw new Exception("tray hide click failed");
            Click(0); if (!IsWindowVisible(overlay.Handle)) throw new Exception("tray show click failed");
            Click(1); var settings = Field<SettingsDialog>(context, "settingsDialog");
            if (!settings.Visible) throw new Exception("tray settings click failed"); settings.Close(); Pump(50);
            Click(2); var history = Field<HistoryDialog>(context, "historyDialog");
            if (!history.Visible) throw new Exception("tray history click failed"); history.Close(); Pump(50);
            Click(4); if (!overlay.IsDisposed) throw new Exception("tray exit click failed");
        }
        finally { context.ExitThread(); Cursor.Position = cursor; if (Directory.Exists(folder)) Directory.Delete(folder, true); }
    }
    internal static void Run()
    {
        Exception? error = null;
        var thread = new Thread(() =>
        {
            try
            {
                Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
                if (Overlay.FormatTime(3599) != "00:59" || Overlay.FormatTime(3600) != "01:00" || Overlay.FormatTime(90061) != "25:01") throw new Exception("overlay hours/minutes format");
                using var overlay = new Overlay();
                overlay.UpdateDisplay("01:23", new Settings());
                Application.DoEvents();
                int style = GetWindowLong(overlay.Handle, -20);
                if ((style & 0x08080020) != 0x08080020) throw new Exception("overlay layered/transparent/noactivate styles");
                if (!overlay.TopMost || overlay.ShowInTaskbar) throw new Exception("overlay topmost/taskbar");
                if (SendMessage(overlay.Handle, 0x84, IntPtr.Zero, IntPtr.Zero).ToInt64() != -1) throw new Exception("overlay hit test");
                if (Math.Abs(overlay.Opacity - .25) > .001) throw new Exception("overlay opacity");
                var hidden = new Settings { OverlayVisible = false };
                for (int i = 0; i < 3; i++)
                {
                    overlay.UpdateDisplay("01:23", hidden);
                    Application.DoEvents();
                    if (overlay.Visible || IsWindowVisible(overlay.Handle)) throw new Exception("overlay should remain hidden during updates");
                }
                overlay.UpdateDisplay("01:23", new Settings());
                Application.DoEvents();
                if (!overlay.Visible || !IsWindowVisible(overlay.Handle)) throw new Exception("overlay should reappear");
                using var session = new SessionWindow(); _ = session.Handle;
                _ = Native.InputTime(DateTimeOffset.UtcNow); _ = Native.SessionAvailable();
                using var settings = new SettingsDialog(new Settings()); settings.Show(); Application.DoEvents(); settings.Close();
                using var history = new HistoryDialog(() => []); history.Show(); Application.DoEvents(); history.Close();
                overlay.Hide();
                CheckTray();
            }
            catch (Exception ex) { error = ex; }
        });
        thread.SetApartmentState(ApartmentState.STA); thread.Start(); thread.Join();
        if (error != null) throw new Exception("UI checks", error);
    }
}
