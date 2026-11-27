using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

class MotionMeter : Form
{
    [DllImport("user32.dll")]
    static extern bool SetProcessDpiAwarenessContext(IntPtr dpi);
    [DllImport("user32.dll")]
    static extern bool SetWindowPos(IntPtr w, IntPtr z, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    static extern bool GetWindowRect(IntPtr w, out RECT rect);
    [DllImport("winmm.dll")]
    static extern uint timeBeginPeriod(uint v);
    [DllImport("winmm.dll")]
    static extern uint timeEndPeriod(uint v);
    [StructLayout(LayoutKind.Sequential)]
    struct RECT
    {
        public int left, top, right, bottom;
    }
    protected override bool ShowWithoutActivation
    {
        get {
            return true;
        }
    }
    protected override CreateParams CreateParams
    {
        get {
            var p = base.CreateParams;
            p.ExStyle |= 0x08000080;
            return p;
        }
    }
    static readonly string root = AppDomain.CurrentDomain.BaseDirectory;
    readonly Rectangle area;
    readonly string device;
    readonly Image[] fiducials;
    readonly Font font = new Font("Segoe UI", 25, FontStyle.Bold);
    readonly Brush dark = new SolidBrush(Color.FromArgb(16, 21, 30));
    readonly Brush track = new SolidBrush(Color.FromArgb(26, 32, 43));
    readonly Brush light = new SolidBrush(Color.FromArgb(244, 244, 244));
    readonly Brush blue = new SolidBrush(Color.FromArgb(45, 123, 209));
    readonly Brush aqua = new SolidBrush(Color.FromArgb(66, 207, 194));
    readonly List<string> events = new List<string>();
    readonly List<string> paints = new List<string>();
    string phase = "starting";
    int code;
    long origin;
    volatile bool closing;
    const double calStart = 1, slowStart = 3.88, staticStart = 5.8, motionStart = 23.8, motionEnd = 83.8;
    MotionMeter(Screen s)
    {
        area = s.Bounds;
        device = s.DeviceName;
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.Manual;
        AutoScaleMode = AutoScaleMode.None;
        ShowInTaskbar = false;
        TopMost = true;
        DoubleBuffered = true;
        Bounds = area;
        Text = "P4 frame rate";
        fiducials = new[] { 21, 33, 9, 46 }
                        .Select(i => Image.FromFile(Path.Combine(root, "mark-" + i + ".png")))
                        .ToArray();
    }
    void eventLog(string name)
    {
        events.Add(name + "," + Stopwatch.GetTimestamp() + "," + DateTime.UtcNow.ToFileTimeUtc() + "," +
                   Stopwatch.Frequency);
    }
    static int[] Slots(int id)
    {
        int a = id & 15, b = (id >> 4) & 15, c = (id >> 8) & 15;
        return new[] { (5 * a + 3) & 15, (7 * a + 5 * b + 3 * c + 9) & 15,
                       (3 * b + 1) & 15, (5 * b + 3 * c + 6) & 15,
                       (7 * c + 2) & 15, (3 * c + 4) & 15 };
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.ScaleTransform(ClientSize.Width / 1440f, ClientSize.Height / 900f);
        g.Clear(Color.FromArgb(15, 23, 36));
        int[] fx = { 26, 1294, 26, 1294 }, fy = { 26, 26, 754, 754 };
        for (int j = 0; j < 4; j++)
        {
            g.FillRectangle(Brushes.White, fx[j] - 12, fy[j] - 12, 144, 144);
            g.DrawImage(fiducials[j], fx[j], fy[j], 120, 120);
        }
        g.DrawString("Windows extended desktop", font, Brushes.White, 188, 38);
        string title = phase == "motion" ? "60 Hz motion" : phase == "static" ? "Static" : "Optical timing";
        g.DrawString(title + "   " + code.ToString("D4"), SystemFonts.CaptionFont, Brushes.White, 195, 99);
        int[] pos = Slots(code);
        for (int i = 0; i < 6; i++)
        {
            int y = 180 + 82 * i;
            g.FillRectangle(dark, 72, y, 1296, 62);
            int cell = phase == "cal" ? code : pos[i];
            g.FillRectangle(light, 82 + 80 * cell, y + 4, 72, 54);
        }
        g.FillRectangle(track, 180, 716, 1080, 106);
        int marker = phase == "motion" || phase == "hold" ? code : 0;
        for (int j = 0; j < 18; j++)
        {
            int h = 16 + (int)(28 * (1 + Math.Sin(marker * .05 + j * .49)));
            g.FillRectangle(j % 3 == 0 ? aqua : blue, 202 + j * 57, 803 - h, 37, h);
        }
        g.FillRectangle(light, 189 + (marker * 9) % 967, 824, 78, 6);
        paints.Add(phase + "," + code + "," + Stopwatch.GetTimestamp() + "," +
                   DateTime.UtcNow.ToFileTimeUtc());
    }
    bool WaitUntil(double seconds)
    {
        long until = origin + (long)(Stopwatch.Frequency * seconds);
        while (!closing)
        {
            double remain = (until - Stopwatch.GetTimestamp()) / (double)Stopwatch.Frequency;
            if (remain <= 0)
                return true;
            if (remain > .002)
                Thread.Sleep((int)Math.Max(1, Math.Min(20, remain * 1000 - 1)));
            else
                Thread.SpinWait(64);
        }
        return false;
    }
    void Set(string ph, int v)
    {
        if (closing)
            return;
        Invoke((Action)delegate {
            if (phase != ph)
            {
                phase = ph;
                eventLog(ph);
            }
            code = v;
            Invalidate();
            Update();
        });
    }
    void Animate()
    {
        try
        {
            if (!WaitUntil(calStart))
                return;
            for (int k = 0; k < 16; k++)
            {
                if (!WaitUntil(calStart + k * .18))
                    return;
                Set("cal", k);
            }
            for (int k = 0; k < 12; k++)
            {
                if (!WaitUntil(slowStart + k * .16))
                    return;
                Set("slow", 3610 + (k * 31) % 440);
            }
            if (!WaitUntil(staticStart))
                return;
            Set("static", 0);
            int next = 0;
            while (next < 3600 && WaitUntil(motionStart + next / 60.0))
            {
                int age = (int)Math.Floor(
                    ((Stopwatch.GetTimestamp() - origin) / (double)Stopwatch.Frequency - motionStart) * 60);
                int tick = Math.Max(next, age);
                if (tick >= 3600)
                    break;
                Set("motion", tick + 1);
                next = tick + 1;
            }
            if (!WaitUntil(motionEnd))
                return;
            Set("hold", 3600);
            if (WaitUntil(motionEnd + 2))
                BeginInvoke((Action)Close);
        }
        catch (InvalidOperationException)
        {
            if (!closing)
                throw;
        }
    }
    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        if (!SetWindowPos(Handle, new IntPtr(-1), area.X, area.Y, area.Width, area.Height, 0x10))
            throw new Exception("Cannot position window");
        RECT w;
        GetWindowRect(Handle, out w);
        if (Screen.FromHandle(Handle).DeviceName != device || w.left != area.X || w.top != area.Y ||
            w.right - w.left != area.Width || w.bottom - w.top != area.Height)
            throw new Exception("Window is outside the requested extended desktop");
        origin = Stopwatch.GetTimestamp();
        eventLog("start");
        Invalidate();
        Update();
        new Thread(Animate) { IsBackground = true }.Start();
    }
    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        closing = true;
        eventLog("closed");
        File.WriteAllLines(Path.Combine(root, "events.csv"),
                           new[] { "phase,qpc,utc_filetime,qpc_hz" }.Concat(events));
        File.WriteAllLines(Path.Combine(root, "paint.csv"),
                           new[] { "phase,id,qpc,utc_filetime" }.Concat(paints));
        foreach (var f in fiducials)
            f.Dispose();
        font.Dispose();
        dark.Dispose();
        light.Dispose();
        track.Dispose();
        blue.Dispose();
        aqua.Dispose();
        base.OnFormClosed(e);
    }
    [STAThread]
    static int Main(string[] args)
    {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        var s = Screen.AllScreens.SingleOrDefault(x => x.DeviceName == args[0] && !x.Primary);
        if (s == null)
            return 1;
        timeBeginPeriod(1);
        try
        {
            Application.Run(new MotionMeter(s));
        }
        finally
        {
            timeEndPeriod(1);
        }
        return 0;
    }
}
