namespace TimeCounter;
internal sealed class CounterContext : ApplicationContext
{
    readonly Settings settings;
    readonly Ledger ledger;
    readonly Overlay overlay = new();
    readonly SessionWindow events = new();
    readonly NotifyIcon tray;
    readonly System.Windows.Forms.Timer timer = new() { Interval = 1000 };
    ApiServer? api;
    DateTimeOffset previous = DateTimeOffset.UtcNow, previousInput;
    bool available, suspended, closing;
    int ticks;
    SettingsDialog? settingsDialog;
    HistoryDialog? historyDialog;
    public CounterContext()
    {
        settings = Settings.Load(); settings.Save();
        ledger = new Ledger(Path.Combine(AppLog.DirectoryPath, "history.json"));
        previousInput = Native.InputTime(previous); available = Native.SessionAvailable();
        var menu = new ContextMenuStrip();
        menu.Items.Add("Скрыть / показать цифры", null, (_, _) => { settings.OverlayVisible = !settings.OverlayVisible; settings.Save(); RefreshOverlay(); });
        menu.Items.Add("Настройки", null, (_, _) => ShowSettings());
        menu.Items.Add("История", null, (_, _) => ShowHistory());
        menu.Items.Add(new ToolStripSeparator()); menu.Items.Add("Выйти", null, (_, _) => ExitThread());
        tray = new NotifyIcon { Icon = SystemIcons.Application, Text = "TimeCounter", ContextMenuStrip = menu, Visible = true };
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
        overlay.UpdateDisplay(HistoryDialog.Duration(today.ActiveSeconds), settings);
        tray.Text = "TimeCounter " + HistoryDialog.Duration(today.ActiveSeconds) + (available && previous - previousInput < TimeSpan.FromMinutes(settings.IdleMinutes) ? " — активно" : " — неактивно");
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
        settingsDialog = new SettingsDialog(settings);
        settingsDialog.FormClosed += (_, _) => { settingsDialog = null; if (oldPort != settings.Port || api == null) { api?.Dispose(); api = null; StartApi(); } RefreshOverlay(); };
        settingsDialog.Show();
    }
    void ShowHistory()
    {
        if (historyDialog != null) { historyDialog.Activate(); return; }
        historyDialog = new HistoryDialog(() => ledger.History(historyDialog?.Days ?? 30, DateTimeOffset.UtcNow, TimeZoneInfo.Local));
        historyDialog.FormClosed += (_, _) => historyDialog = null; historyDialog.Show();
    }
    protected override void ExitThreadCore()
    {
        if (closing) return; closing = true; timer.Stop();
        try { Tick(); ledger.Save(); settings.Save(); } catch (Exception ex) { AppLog.Write(ex); MessageBox.Show(ex.Message, "TimeCounter: ошибка сохранения"); }
        Microsoft.Win32.SystemEvents.SessionEnding -= SessionEnding;
        api?.Dispose(); tray.Visible = false; tray.Dispose(); settingsDialog?.Dispose(); historyDialog?.Dispose(); overlay.Dispose(); events.Dispose(); timer.Dispose(); base.ExitThreadCore();
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
