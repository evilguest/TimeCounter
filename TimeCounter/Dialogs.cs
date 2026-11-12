namespace TimeCounter;
internal sealed class SettingsDialog : Form
{
    readonly Settings s;
    readonly NumericUpDown idle = Number(1, 1440), size = Number(8, 500), transparency = Number(0, 99), x = Number(0, 10000), y = Number(0, 10000), port = Number(1024, 65535), monitor = Number(1, 32);
    readonly ComboBox font = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    readonly ComboBox corner = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 260 };
    readonly Button color = new() { Text = "Выбрать цвет", AutoSize = true };
    readonly CheckBox auto = new() { Text = "Запускать вместе с Windows", AutoSize = true };
    int argb;
    static NumericUpDown Number(int min, int max) => new() { Minimum = min, Maximum = max, Width = 120 };
    public SettingsDialog(Settings settings)
    {
        s = settings; argb = s.ColorArgb; Text = "TimeCounter — настройки";
        AutoScaleMode = AutoScaleMode.Dpi; ClientSize = new Size(620, 630); MinimumSize = new Size(640, 650);
        var table = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoScroll = true, Padding = new Padding(16) };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 48)); table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 52));
        void Row(string label, Control c) { int r = table.RowCount++; table.RowStyles.Add(new RowStyle(SizeType.AutoSize)); table.Controls.Add(new Label { Text = label, AutoSize = true, Margin = new Padding(3, 8, 3, 10) }, 0, r); c.Margin = new Padding(3, 5, 3, 5); table.Controls.Add(c, 1, r); }
        font.Items.AddRange(FontFamily.Families.Select(f => (object)f.Name).ToArray());
        if (!font.Items.Contains(s.FontName)) font.Items.Add(s.FontName);
        font.SelectedItem = s.FontName; size.Value = (decimal)s.FontSize; idle.Value = s.IdleMinutes; transparency.Value = s.Transparency;
        x.Value = Math.Clamp(s.OffsetX, 0, 10000); y.Value = Math.Clamp(s.OffsetY, 0, 10000); port.Value = s.Port; monitor.Value = Math.Clamp(s.Monitor + 1, 1, 32);
        corner.Items.AddRange(["Справа сверху", "Слева сверху", "Справа снизу", "Слева снизу"]); corner.SelectedItem = s.Corner; if (corner.SelectedIndex < 0) corner.SelectedIndex = 0;
        color.BackColor = Color.FromArgb(argb);
        color.Click += (_, _) => { using var d = new ColorDialog { Color = Color.FromArgb(argb), FullOpen = true }; if (d.ShowDialog(this) == DialogResult.OK) { argb = d.Color.ToArgb(); color.BackColor = d.Color; } };
        auto.Checked = Settings.AutoStart;
        Row("Таймаут без ввода, минут", idle); Row("Шрифт", font); Row("Размер, pt", size); Row("Цвет цифр", color); Row("Прозрачность, %", transparency);
        Row("Угол экрана", corner); Row("Экран (с 1)", monitor); Row("Отступ по горизонтали, px", x); Row("Отступ по вертикали, px", y); Row("Порт HTTP (все IP)", port);
        Row("Токен API", new TextBox { Text = s.ApiToken, ReadOnly = true, Width = 280 }); Row("Автозагрузка", auto);
        Row("", new Label { Text = "HTTP передаёт токен без шифрования.\nДля доступа используйте доверенную сеть или SSH/VPN.", AutoSize = true });
        var save = new Button { Text = "Сохранить", AutoSize = true };
        save.Click += (_, _) =>
        {
            try
            {
                // Probe changed port before persisting settings.
                if ((int)port.Value != s.Port) { var probe = new System.Net.Sockets.TcpListener(System.Net.IPAddress.IPv6Any, (int)port.Value); probe.Server.DualMode = true; try { probe.Start(); } finally { probe.Stop(); } }
                Settings.AutoStart = auto.Checked;
                s.IdleMinutes = (int)idle.Value; s.FontName = font.Text; s.FontSize = (float)size.Value; s.ColorArgb = argb; s.Transparency = (int)transparency.Value;
                s.Corner = corner.Text; s.Monitor = (int)monitor.Value - 1; s.OffsetX = (int)x.Value; s.OffsetY = (int)y.Value; s.Port = (int)port.Value;
                s.Save(); DialogResult = DialogResult.OK; Close();
            }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Настройки не сохранены"); }
        };
        Row("", save); Controls.Add(table); AcceptButton = save;
    }
}
internal sealed class HistoryDialog : Form
{
    readonly DataGridView grid = new() { Dock = DockStyle.Fill, ReadOnly = true, AllowUserToAddRows = false, AllowUserToDeleteRows = false, RowHeadersVisible = false, AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill, SelectionMode = DataGridViewSelectionMode.FullRowSelect };
    readonly System.Windows.Forms.Timer timer = new() { Interval = 5000 };
    readonly Func<List<DayRow>> get;
    internal static string Duration(double seconds) { var t = TimeSpan.FromSeconds(Math.Floor(seconds)); return $"{(int)t.TotalHours:00}:{t.Minutes:00}:{t.Seconds:00}"; }
    public HistoryDialog(Func<List<DayRow>> get)
    {
        this.get = get; Text = "TimeCounter — история"; ClientSize = new Size(760, 530);
        grid.Columns.Add("date", "Дата"); grid.Columns.Add("active", "Активно"); grid.Columns.Add("inactive", "Неактивно"); grid.Columns.Add("uncounted", "Непосчитано");
        var days = new NumericUpDown { Minimum = 0, Maximum = 3660, Value = 30, Width = 90 };
        Days = 30; var top = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 45, Padding = new Padding(8) };
        top.Controls.Add(new Label { Text = "Предыдущих дней (+ сегодня):", AutoSize = true }); top.Controls.Add(days);
        days.ValueChanged += (_, _) => { Days = (int)days.Value; RefreshRows(); };
        Controls.Add(grid); Controls.Add(top); timer.Tick += (_, _) => RefreshRows(); timer.Start(); RefreshRows();
    }
    public int Days { get; private set; }
    void RefreshRows() { grid.Rows.Clear(); foreach (var d in get()) grid.Rows.Add(d.Date, Duration(d.ActiveSeconds), Duration(d.InactiveSeconds), Duration(d.UncountedSeconds)); }
    protected override void Dispose(bool disposing) { if (disposing) timer.Dispose(); base.Dispose(disposing); }
}
