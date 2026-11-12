using System.Runtime.InteropServices;
using TimeCounter;
internal static class UiChecks
{
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    internal static void Run()
    {
        Exception? error = null;
        var thread = new Thread(() =>
        {
            try
            {
                Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
                using var overlay = new Overlay();
                overlay.UpdateDisplay("01:23:45", new Settings());
                Application.DoEvents();
                int style = GetWindowLong(overlay.Handle, -20);
                if ((style & 0x08080020) != 0x08080020) throw new Exception("overlay layered/transparent/noactivate styles");
                if (!overlay.TopMost || overlay.ShowInTaskbar) throw new Exception("overlay topmost/taskbar");
                if (SendMessage(overlay.Handle, 0x84, IntPtr.Zero, IntPtr.Zero).ToInt64() != -1) throw new Exception("overlay hit test");
                if (Math.Abs(overlay.Opacity - .25) > .001) throw new Exception("overlay opacity");
                using var session = new SessionWindow(); _ = session.Handle;
                _ = Native.InputTime(DateTimeOffset.UtcNow); _ = Native.SessionAvailable();
                using var settings = new SettingsDialog(new Settings()); settings.Show(); Application.DoEvents(); settings.Close();
                using var history = new HistoryDialog(() => []); history.Show(); Application.DoEvents(); history.Close();
            }
            catch (Exception ex) { error = ex; }
        });
        thread.SetApartmentState(ApartmentState.STA); thread.Start(); thread.Join();
        if (error != null) throw new Exception("UI checks", error);
    }
}
