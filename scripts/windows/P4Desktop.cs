using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

sealed class DxMonitor : IDisposable
{
    [DllImport("GpuCapture.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern int capture_open(string device, int x, int y, int w, int h, out IntPtr capture);
    [DllImport("GpuCapture.dll", ExactSpelling = true)]
    static extern int capture_next(IntPtr capture, IntPtr dst, int length);
    [DllImport("GpuCapture.dll", ExactSpelling = true)]
    static extern void capture_close(IntPtr capture);
    IntPtr cap, buffer;
    public Bitmap Image { get; private set; }
    const int bytes = 800 * 1280 * 4;
    public DxMonitor(string name, Rectangle r)
    {
        int hr = capture_open(name, r.X, r.Y, r.Width, r.Height, out cap);
        if (hr < 0)
            throw new Exception("DXGI open HRESULT 0x" + hr.ToString("X8"));
        try
        {
            buffer = Marshal.AllocHGlobal(bytes);
            Image = new Bitmap(800, 1280, 800 * 4, PixelFormat.Format32bppRgb, buffer);
        }
        catch
        {
            Dispose();
            throw;
        }
    }
    [DllImport("GpuCapture.dll", ExactSpelling = true)]
    static extern int capture_cursor_separate(IntPtr cap);
    public bool SeparatePointer
    {
        get {
            return capture_cursor_separate(cap) != 0;
        }
    }
    public bool Next()
    {
        int hr = capture_next(cap, buffer, bytes);
        if (hr < 0)
            throw new Exception("DXGI acquire HRESULT 0x" + hr.ToString("X8"));
        return hr == 0;
    }
    public void Dispose()
    {
        if (Image != null)
            Image.Dispose();
        if (buffer != IntPtr.Zero)
            Marshal.FreeHGlobal(buffer);
        if (cap != IntPtr.Zero)
            capture_close(cap);
    }
}

static class P4Desktop
{
    [DllImport("user32.dll")]
    static extern bool SetProcessDpiAwarenessContext(IntPtr mode);
    [StructLayout(LayoutKind.Sequential)]
    struct CursorInfo
    {
        public int size, flags;
        public IntPtr handle;
        public Point pos;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IconInfo
    {
        public int isIcon;
        public uint hotX, hotY;
        public IntPtr mask, color;
    }
    [DllImport("user32.dll")]
    static extern bool GetCursorInfo(ref CursorInfo info);
    [DllImport("user32.dll")]
    static extern bool GetIconInfo(IntPtr icon, out IconInfo info);
    [DllImport("gdi32.dll")]
    static extern bool DeleteObject(IntPtr obj);
    static string Local(string name)
    {
        return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, name);
    }
    static PointF PanelPoint(Rectangle area, float x, float y)
    {
        return new PointF((y - area.Top) * 800f / area.Height,
                          1280f - (x - area.Left) * 1280f / area.Width);
    }
    static int Run(string target, Rectangle area)
    {
        var screen = Screen.AllScreens.SingleOrDefault(d => d.DeviceName == target && !d.Primary &&
                                                            d.Bounds == area);
        if (screen == null)
            throw new InvalidOperationException("Extended output geometry changed");
        var codec = ImageCodecInfo.GetImageEncoders().Single(c => c.MimeType == "image/jpeg");
        var stop = Local("stop.request");
        var clock = Stopwatch.StartNew();
        using (var capture = new DxMonitor(target, area))
        using (var overlay = new Bitmap(800, 1280, PixelFormat.Format32bppRgb))
        using (var paint = Graphics.FromImage(overlay))
        using (var quality = new EncoderParameters(1))
        using (var mem = new MemoryStream(131072))
        using (var output = new BinaryWriter(Console.OpenStandardOutput()))
        {
            quality.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 72L);
            bool hasBase = false, lastShown = false;
            Point lastPoint = Point.Empty, hotspot = Point.Empty;
            IntPtr cachedHandle = IntPtr.Zero;
            Bitmap sprite = null;
            long lastSend = -2000;
            try
            {
                while (!File.Exists(stop))
                {
                    bool changed = capture.Next();
                    hasBase |= changed;
                    if (!hasBase)
                    {
                        if (clock.ElapsedMilliseconds > 8000)
                            throw new IOException("No initial frame from the virtual display");
                        continue;
                    }
                    var ci = new CursorInfo { size = Marshal.SizeOf(typeof(CursorInfo)) };
                    bool shown = capture.SeparatePointer && GetCursorInfo(ref ci) && (ci.flags & 1) != 0 &&
                                 area.Contains(ci.pos);
                    bool pointerChanged =
                        shown != lastShown || (shown && (ci.pos != lastPoint || ci.handle != cachedHandle));
                    if (shown && ci.handle != cachedHandle)
                    {
                        if (sprite != null)
                            sprite.Dispose();
                        sprite = null;
                        cachedHandle = ci.handle;
                        IconInfo ii;
                        if (GetIconInfo(ci.handle, out ii))
                        {
                            hotspot = new Point((int)ii.hotX, (int)ii.hotY);
                            if (ii.mask != IntPtr.Zero)
                                DeleteObject(ii.mask);
                            if (ii.color != IntPtr.Zero)
                                DeleteObject(ii.color);
                            try
                            {
                                using (var icon = Icon.FromHandle(ci.handle))
                                    sprite = icon.ToBitmap();
                            }
                            catch (ArgumentException)
                            {
                                sprite = null;
                            }
                        }
                    }
                    lastPoint = ci.pos;
                    lastShown = shown;
                    if (changed || pointerChanged || mem.Length == 0)
                    {
                        Bitmap frame = capture.Image;
                        if (shown && sprite != null)
                        {
                            paint.DrawImageUnscaled(frame, 0, 0);
                            var q = Point.Subtract(ci.pos, new Size(hotspot));
                            var pts = new[] { PanelPoint(area, q.X, q.Y),
                                              PanelPoint(area, q.X + sprite.Width, q.Y),
                                              PanelPoint(area, q.X, q.Y + sprite.Height) };
                            paint.DrawImage(sprite, pts);
                            paint.Flush();
                            frame = overlay;
                        }
                        mem.Position = 0;
                        mem.SetLength(0);
                        frame.Save(mem, codec, quality);
                    }
                    else if (clock.ElapsedMilliseconds - lastSend < 1000)
                        continue;
                    output.Write((int)mem.Length);
                    output.Write(mem.GetBuffer(), 0, (int)mem.Length);
                    output.Flush();
                    lastSend = clock.ElapsedMilliseconds;
                }
            }
            finally
            {
                if (sprite != null)
                    sprite.Dispose();
            }
        }
        return 0;
    }
    [STAThread]
    static int Main(string[] args)
    {
        try
        {
            SetProcessDpiAwarenessContext(new IntPtr(-4));
            return Run(args[0], new Rectangle(int.Parse(args[1]), int.Parse(args[2]), int.Parse(args[3]),
                                              int.Parse(args[4])));
        }
        catch (Exception err)
        {
            Console.Error.WriteLine(err.Message);
            return 1;
        }
    }
}
