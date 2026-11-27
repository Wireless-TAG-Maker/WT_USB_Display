using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;
using W = System.Windows;

sealed partial class DisplayApp : Form
{
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunValue = "WTUSBDisplay";
    const string ShowEvent = @"Local\WTUSBDisplay.Show";
    const string QuitEvent = @"Local\WTUSBDisplay.Quit";
    readonly string session = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                           "WTUSBDisplay", "session");
    readonly EventWaitHandle showEvent, quitEvent;
    readonly ToolStripMenuItem trayOpen = new ToolStripMenuItem();
    readonly NotifyIcon tray = new NotifyIcon();
    readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
    Process worker;
    string offeredDisplay, currentStatus = "detecting", statusError;
    bool stopping, exiting;

    [DllImport("user32.dll")]
    static extern bool SetProcessDpiAwarenessContext(IntPtr context);

    [STAThread]
    static void Main(string[] args)
    {
        bool first, background = Array.IndexOf(args, "--tray") >= 0;
        using (var show = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEvent))
        using (var quit = new EventWaitHandle(false, EventResetMode.AutoReset, QuitEvent))
        using (var instance = new Mutex(false, @"Local\WTUSBDisplay.App", out first))
        {
            if (!first)
            {
                if (!background) show.Set();
                return;
            }
            SetProcessDpiAwarenessContext(new IntPtr(-4));
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            using (var app = new DisplayApp(show, quit))
            {
                if (!background) app.Show();
                app.StartConnection(!background);
                Application.Run();
            }
        }
    }

    DisplayApp(EventWaitHandle show, EventWaitHandle quit)
    {
        showEvent = show;
        quitEvent = quit;
        english = ReadLanguage();
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        BuildView();
        connect.Click += delegate {
            if (worker == null) { StartConnection(true); return; }
            if (stopping || offeredDisplay == null) return;
            try
            {
                worker.StandardInput.WriteLine("use\t" + offeredDisplay);
                worker.StandardInput.Flush();
                offeredDisplay = null;
                SetStatus("starting");
            }
            catch (IOException) { SetStatus("connection_ended"); }
        };
        pause.Click += delegate { StopConnection(); };
        settings.Click += delegate { OpenLocation(this, "ms-settings:display"); };

        try { startup.IsChecked = String.Equals(Registry.GetValue(@"HKEY_CURRENT_USER\" + RunKey, RunValue, null)
                                              as string, StartupCommand, StringComparison.OrdinalIgnoreCase); }
        catch (System.Security.SecurityException) { startup.IsEnabled = false; }
        catch (UnauthorizedAccessException) { startup.IsEnabled = false; }
        startup.Checked += SaveStartup;
        startup.Unchecked += SaveStartup;
        var menu = new ContextMenuStrip();
        trayOpen.Click += delegate { ShowApp(); };
        menu.Items.Add(trayOpen);
        tray.Icon = Icon;
        tray.ContextMenuStrip = menu;
        tray.DoubleClick += delegate { ShowApp(); };
        ApplyLanguage();
        tray.Visible = true;
        // Create the window handle even when starting directly in the notification area.
        CreateHandle();
        ApplyAppearance();
        timer.Interval = 400;
        timer.Tick += delegate { Tick(); };
        timer.Start();
    }

    bool restoringStartup;
    void SaveStartup(object sender, W.RoutedEventArgs args)
    {
        if (restoringStartup) return;
        bool enabled = startup.IsChecked == true;
        try {
            using (var key = Registry.CurrentUser.CreateSubKey(RunKey)) {
                if (enabled) key.SetValue(RunValue, StartupCommand);
                else key.DeleteValue(RunValue, false);
            }
        }
        catch (Exception ex) {
            restoringStartup = true;
            try { startup.IsChecked = !enabled; } finally { restoringStartup = false; }
            MessageBox.Show(this, T("无法保存登录启动设置。", "Could not save the sign-in setting.") + "\n" + ex.Message, Product);
        }
    }

    static string StartupCommand
    {
        get { return "\"" + Path.Combine(AppDomain.CurrentDomain.BaseDirectory, EnglishProductName + ".exe") + "\" --tray"; }
    }

    void ShowApp()
    {
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
    }

    void SetStatus(string code, string error = null)
    {
        currentStatus = code;
        statusError = error;
        RenderStatus();
    }

    void StatusText(string zhTitle, string enTitle, string zhDetail, string enDetail, params object[] args)
    {
        status.Text = T(zhTitle, enTitle);
        detail.Text = String.Format(T(zhDetail, enDetail), args);
        tray.Text = Product + " · " + status.Text;
    }

    void WorkerStatus(string code)
    {
        string[] offer = code.Split('\t');
        bool available = offer.Length == 3 && offer[0] == "existing_available";
        offeredDisplay = available ? offer[1] + "\t" + offer[2] : null;
        SetStatus(available ? "existing_available" : code);
    }

    void RenderStatus()
    {
        connect.Content = currentStatus == "start_error" ? T("重试连接", "Retry connection") : T("连接副屏", "Connect display");
        connect.Visibility = worker == null || offeredDisplay != null ? W.Visibility.Visible : W.Visibility.Collapsed;
        pause.Content = T("暂停连接", "Pause connection");
        pause.Visibility = worker != null ? W.Visibility.Visible : W.Visibility.Collapsed;
        pause.IsEnabled = !stopping;
        ShowSignal();
        switch (currentStatus)
        {
            case "existing_available":
                if (offeredDisplay == null) break;
                var display = offeredDisplay.Split('\t');
                StatusText("检测到已有扩展屏", "Existing display found",
                    "点击“连接副屏”，即可将 {0}（{1}）的画面显示在副屏上。",
                    "Choose “Connect display” to show {0} ({1}) on the USB display.",
                    display[0].Replace(@"\\.\", ""), display[1]); break;
            case "detecting":
                StatusText("正在检测副屏", "Looking for the display", "正在查找已连接的设备。", "Looking for a connected USB display."); break;
            case "unplugged":
                StatusText("等待连接副屏", "Waiting for the display", "用支持数据传输的 USB 线将电脑连接到副屏的 HUSB 接口。",
                    "Connect your PC to the HUSB port on the display with a USB data cable."); break;
            case "starting":
                StatusText("正在准备扩展屏", "Preparing the extended display", "正在为副屏创建独立的 Windows 桌面。",
                    "Setting up a separate Windows desktop for the USB display."); break;
            case "running":
                StatusText("正在连接", "Connecting", "扩展桌面已就绪，正在等待副屏确认显示第一帧。",
                    "The extended desktop is ready. Waiting for the display to confirm its first frame."); break;
            case "extend_display":
                StatusText("请启用扩展显示", "Extend the desktop", "打开“显示器排列”，选择“扩展这些显示器”并保留更改。应用会继续连接。",
                    "Open “Arrange displays”, choose “Extend these displays” and keep the changes. The app will continue connecting."); break;
            case "streaming":
                StatusText("副屏已连接", "Display connected", "将需要在副屏使用的窗口拖到扩展屏上。",
                    "Drag a window onto the extended display to use it there."); break;
            case "multiple_devices":
                StatusText("检测到多台副屏", "Multiple displays detected", "请只保留一台 {0}，应用会自动连接。",
                    "Leave one {0} connected. The app will connect automatically.", Product); break;
            case "slow_usb":
                StatusText("USB 速度不足", "USB connection is too slow", "请使用支持 USB 2.0 高速传输的数据线和接口。",
                    "Use a data cable and port that support USB 2.0 High-Speed."); break;
            case "usb_error":
                StatusText("副屏暂未就绪", "USB display unavailable", "请检查 HUSB 连线、配套显示固件，并关闭其他正在投屏的程序。应用会自动重试。",
                    "Check the HUSB cable and display firmware, and close any other app using the display. The app will retry."); break;
            case "driver_error":
                StatusText("显示驱动未就绪", "Display driver unavailable", "请重新运行 {0} 安装包以检查驱动。",
                    "Run the {0} installer again to check the driver.", Product); break;
            case "other_display":
                StatusText("检测到多块虚拟扩展屏", "Multiple virtual displays found", "请只启用要投送的那块虚拟扩展屏。",
                    "Leave only the virtual display you want to show enabled."); break;
            case "incompatible_display":
                StatusText("请调整虚拟扩展屏", "Adjust the virtual display", "在“显示器排列”中将虚拟屏设为独立的 16:10 横向副屏。",
                    "In “Arrange displays”, set the virtual screen as a separate 16:10 landscape secondary display."); break;
            case "waiting_for_separate_landscape_monitor":
                StatusText("请调整显示器排列", "Adjust the display layout", "将副屏设为横向的扩展显示器，并将主显示器保留在原屏幕上。",
                    "Extend the desktop to the USB display in landscape and keep your original screen as the main display."); break;
            case "no_desktop":
                StatusText("等待 Windows 桌面", "Waiting for the Windows desktop", "请登录本机桌面，应用将自动重试。",
                    "Sign in to the local desktop. The app will retry."); break;
            case "display_error":
                StatusText("扩展屏尚未就绪", "Extended display unavailable", "请打开“显示器排列”查看副屏。如果显示 1|2，请选择“扩展这些显示器”。",
                    "Open “Arrange displays” to check the USB display. If you see 1|2, choose “Extend these displays”."); break;
            case "reconnecting":
                StatusText("正在重新连接", "Reconnecting", "请检查副屏连接和 Windows 显示设置，应用会自动重试。",
                    "Check the USB connection and Windows display settings. The app will retry."); break;
            case "disconnecting":
                StatusText("正在断开", "Disconnecting", "正在释放本次连接使用的扩展屏。", "Releasing the extended display used by this connection."); break;
            case "paused":
                StatusText("连接已暂停", "Connection paused", "点击“连接副屏”可恢复自动连接。", "Choose “Connect display” to resume automatic connection."); break;
            case "other_session":
                StatusText("已有投屏任务", "Another display session is running", "结束原有的投屏任务后，点击“连接副屏”。",
                    "End the other display session, then choose “Connect display”."); break;
            case "start_error":
                StatusText("无法启动投屏", "Could not start the display", "请重新运行安装包修复应用。\n{0}", "Run the installer again to repair the app.\n{0}", statusError); break;
            case "connection_ended":
                StatusText("连接已结束", "Connection ended", "请重新连接副屏。", "Reconnect the USB display."); break;
            case "stopped": case "error":
                StatusText("投屏程序已退出", "Display session ended", "点击“连接副屏”重试。如果问题持续，请重新运行安装包。",
                    "Choose “Connect display” to try again. If the problem persists, run the installer again."); break;
        }
    }

    void StartConnection(bool autoSelect)
    {
        var process = new Process();
        try
        {
            Directory.CreateDirectory(session);
            string install = AppDomain.CurrentDomain.BaseDirectory;
            process.StartInfo = new ProcessStartInfo(Path.Combine(install, @"python\python.exe"),
                "-B -u \"" + Path.Combine(install, "serve.py") + "\" --watch" + (autoSelect ? " --connect" : "")) {
                WorkingDirectory = session, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardInput = true, RedirectStandardOutput = true
            };
            process.StartInfo.EnvironmentVariables[ChromaPreference.FollowApp] = "1";
            process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs line) {
                if (line.Data == null) return;
                try { BeginInvoke((Action)delegate {
                    if (worker == process && !stopping) WorkerStatus(line.Data);
                }); }
                catch (InvalidOperationException) { } // Window already closed.
            };
            process.Start();
            worker = process;
            process.BeginOutputReadLine();
            SetStatus("detecting");
        }
        catch (Exception ex)
        {
            if (worker != null) worker.StandardInput.Close();
            process.Dispose();
            worker = null;
            SetStatus("start_error", ex.Message);
        }
    }

    void StopConnection()
    {
        if (worker == null || stopping) return;
        stopping = true;
        offeredDisplay = null;
        SetStatus("disconnecting");
        try { worker.StandardInput.Close(); }
        catch (IOException) { } // The worker may already have closed the pipe.
    }

    void Tick()
    {
        if (quitEvent.WaitOne(0)) RequestExit();
        if (!exiting && showEvent.WaitOne(0)) ShowApp();
        if (worker == null || !worker.HasExited) return;
        int code = worker.ExitCode;
        worker.Dispose();
        worker = null;
        offeredDisplay = null;
        bool paused = stopping;
        stopping = false;
        if (exiting) { Close(); return; }
        if (paused) SetStatus("paused");
        else SetStatus(code == 2 ? "other_session" : "stopped");
    }

    void RequestExit()
    {
        exiting = true;
        if (worker == null) Close();
        else StopConnection();
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (e.CloseReason == CloseReason.UserClosing && !exiting)
        {
            e.Cancel = true;
            Hide();
        }
        else if (worker != null)
        {
            StopConnection();
            worker.WaitForExit(8000);
        }
        base.OnFormClosing(e);
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        timer.Stop();
        timer.Dispose();
        tray.Visible = false;
        tray.ContextMenuStrip.Dispose();
        tray.Dispose();
        moreMenu.Dispose();
        base.OnFormClosed(e);
        Application.ExitThread();
    }
}
