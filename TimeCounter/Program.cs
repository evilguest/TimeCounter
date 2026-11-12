namespace TimeCounter;
internal sealed class CounterContext : ApplicationContext
{
    readonly Settings settings;
    readonly Action<Settings> saveSettings;
    readonly Ledger ledger;
    readonly Overlay overlay = new();
    readonly SessionWindow events = new();
    readonly NotifyIcon tray;
    readonly Icon appIcon;
    IntPtr activeMenu;
    bool menuOpen;
    readonly System.Windows.Forms.Timer timer = new() { Interval = 1000 };
    ApiServer? api;
    DateTimeOffset previous = DateTimeOffset.UtcNow, previousInput;
    bool available, suspended, closing;
    int ticks;
    SettingsDialog? settingsDialog;
    HistoryDialog? historyDialog;
    public CounterContext(Settings? initialSettings = null, Ledger? initialLedger = null, Action<Settings>? persistSettings = null)
    {
        settings = initialSettings ?? Settings.Load();
        saveSettings = persistSettings ?? (s => s.Save());
        saveSettings(settings);
        ledger = initialLedger ?? new Ledger(Path.Combine(AppLog.DirectoryPath, "history.json"));
        previousInput = Native.InputTime(previous); available = Native.SessionAvailable();
        using (var stream = typeof(CounterContext).Assembly.GetManifestResourceStream("TimeCounter.app.ico")!)
            appIcon = new Icon(stream);
        tray = new NotifyIcon { Icon = appIcon, Text = "TimeCounter", Visible = true };
        tray.MouseUp += (_, e) => { if (e.Button == MouseButtons.Right) ShowTrayMenu(); };
        tray.DoubleClick += (_, _) => ShowHistory();
        events.SessionChanged += code =>
        {
            Tick();
            if (code is 2 or 4 or 6 or 7) available = false;
            else if (code is 1 or 3 or 5 or 8) available = Native.SessionAvailable();
        };
        events.PowerChanged += sleep => { Tick(); suspended = sleep; available = !sleep && Native.SessionAvailable(); };
        _ = events.Handle;
        Microsoft.Win32.SystemEvents.SessionEnding += SessionEnding;
        timer.Tick += (_, _) =>
        {
            try { Tick(); if (++ticks % 15 == 0) ledger.Save(); }
            catch (Exception ex) { AppLog.Write(ex); timer.Stop(); tray.ShowBalloonTip(10000, "TimeCounter: учёт остановлен", ex.Message, ToolTipIcon.Error); }
        };
        StartApi(); RefreshOverlay(); timer.Start();
    }
    internal void ShowTrayMenu()
    {
        if (menuOpen || closing) return;
        activeMenu = Native.CreatePopupMenu();
        if (activeMenu == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        uint command;
        menuOpen = true;
        try
        {
            Native.AppendMenuW(activeMenu, 0, (UIntPtr)1, settings.OverlayVisible ? "Скрыть цифры" : "Показать цифры");
            Native.AppendMenuW(activeMenu, 0, (UIntPtr)2, "Настройки");
            Native.AppendMenuW(activeMenu, 0, (UIntPtr)3, "История");
            Native.AppendMenuW(activeMenu, 0x800, UIntPtr.Zero, null);
            Native.AppendMenuW(activeMenu, 0, (UIntPtr)4, "Выйти");
            var point = Cursor.Position;
            // A tray menu needs a foreground owner and a posted message after closing,
            // including on subsequent opens. Keep it independent of the no-activate overlay.
            Native.SetForegroundWindow(events.Handle);
            command = Native.TrackPopupMenuEx(activeMenu, 0x100 | 0x80 | 0x2, point.X, point.Y, events.Handle, IntPtr.Zero);
        }
        finally
        {
            Native.PostMessageW(events.Handle, 0, UIntPtr.Zero, IntPtr.Zero);
            Native.DestroyMenu(activeMenu); activeMenu = IntPtr.Zero; menuOpen = false;
        }
        switch (command)
        {
            case 1: settings.OverlayVisible = !settings.OverlayVisible; saveSettings(settings); RefreshOverlay(); break;
            case 2: ShowSettings(); break;
            case 3: ShowHistory(); break;
            case 4: ExitThread(); break;
        }
    }
    void SessionEnding(object? sender, Microsoft.Win32.SessionEndingEventArgs e) { Tick(); ledger.Save(); }
    void Tick()
    {
        var now = DateTimeOffset.UtcNow;
        var input = Native.InputTime(now);
        // A delayed timer (sleep/hibernation or stalled UI) cannot establish activity.
        if (now - previous > TimeSpan.FromSeconds(10)) ledger.Add(previous, now, false);
        else if (now > previous)
        {
            if (available && !suspended && input > previous && input <= now)
            {
                Accounting.Record(ledger, previous, input, true, previousInput, TimeSpan.FromMinutes(settings.IdleMinutes));
                Accounting.Record(ledger, input, now, true, input, TimeSpan.FromMinutes(settings.IdleMinutes));
            }
            else Accounting.Record(ledger, previous, now, available && !suspended, input > previous ? previousInput : input, TimeSpan.FromMinutes(settings.IdleMinutes));
        }
        previous = now; previousInput = input;
        available = !suspended && Native.SessionAvailable();
        RefreshOverlay();
    }
    void RefreshOverlay()
    {
        var today = ledger.History(0, DateTimeOffset.UtcNow, TimeZoneInfo.Local)[0];
        // Keep the overlay from changing window order while the tray menu owns input.
        if (!menuOpen) overlay.UpdateDisplay(Overlay.FormatTime(today.ActiveSeconds), settings);
        tray.Text = "TimeCounter " + Overlay.FormatTime(today.ActiveSeconds) + (available && previous - previousInput < TimeSpan.FromMinutes(settings.IdleMinutes) ? " — активно" : " — неактивно");
    }
    void StartApi()
    {
        try { api = new ApiServer(settings.Port, settings.ApiToken, days => { var now = DateTimeOffset.UtcNow; return new { generatedAt = now, timeZone = TimeZoneInfo.Local.Id, days = ledger.History(days, now, TimeZoneInfo.Local) }; }); }
        catch (Exception ex) { AppLog.Write(ex); tray.ShowBalloonTip(10000, "HTTP API не запущен", $"Порт {settings.Port}: {ex.Message}", ToolTipIcon.Warning); }
    }
    void ShowSettings()
    {
        if (settingsDialog != null) { settingsDialog.Activate(); return; }
        int oldPort = settings.Port;
        Tick();
        settingsDialog = new SettingsDialog(settings) { Icon = appIcon };
        settingsDialog.FormClosed += (_, _) => { settingsDialog = null; if (oldPort != settings.Port || api == null) { api?.Dispose(); api = null; StartApi(); } RefreshOverlay(); };
        settingsDialog.Show();
    }
    void ShowHistory()
    {
        if (historyDialog != null) { historyDialog.Activate(); return; }
        historyDialog = new HistoryDialog(() => ledger.History(historyDialog?.Days ?? 30, DateTimeOffset.UtcNow, TimeZoneInfo.Local)) { Icon = appIcon };
        historyDialog.FormClosed += (_, _) => historyDialog = null; historyDialog.Show();
    }
    protected override void ExitThreadCore()
    {
        if (closing) return; closing = true; timer.Stop();
        try { Tick(); ledger.Save(); saveSettings(settings); } catch (Exception ex) { AppLog.Write(ex); MessageBox.Show(ex.Message, "TimeCounter: ошибка сохранения"); }
        Microsoft.Win32.SystemEvents.SessionEnding -= SessionEnding;
        api?.Dispose(); tray.Visible = false; tray.Dispose(); settingsDialog?.Dispose(); historyDialog?.Dispose(); overlay.Dispose(); events.Dispose(); timer.Dispose(); appIcon.Dispose(); base.ExitThreadCore();
    }
}
internal static class Program
{
    [STAThread]
    static void Main()
    {
        using var mutex = new Mutex(true, @"Global\TimeCounter-" + System.Security.Principal.WindowsIdentity.GetCurrent().User!.Value, out bool first);
        if (!first) { MessageBox.Show("TimeCounter уже запущен для этого пользователя."); return; }
        ApplicationConfiguration.Initialize();
        try { Application.Run(new CounterContext()); }
        catch (Exception ex) { AppLog.Write(ex); MessageBox.Show(ex.Message, "TimeCounter: ошибка запуска"); }
    }
}
