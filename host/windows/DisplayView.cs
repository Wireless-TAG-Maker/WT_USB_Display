using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Security;
using System.Windows.Forms;
using System.Windows.Forms.Integration;
using Microsoft.Win32;
using W = System.Windows;
using Ui = System.Windows.Controls;
using Media = System.Windows.Media;
using Shapes = System.Windows.Shapes;

sealed partial class DisplayApp
{
    readonly W.ResourceDictionary design = ReadView<W.ResourceDictionary>("DisplayStyles.xaml");
    readonly Ui.Grid view = ReadView<Ui.Grid>("DisplayView.xaml");
    Ui.TextBlock status, detail, qualityHint;
    Ui.Button connect, pause, settings, more;
    Ui.CheckBox startup;
    Ui.RadioButton standardChoice, fullChoice;
    Shapes.Ellipse signal;
    Ui.Canvas panelWallpaper;
    bool? previousDark, previousContrast;

    static T ReadView<T>(string name)
    {
        using (var stream = typeof(DisplayApp).Assembly.GetManifestResourceStream(name))
            return (T)W.Markup.XamlReader.Load(stream);
    }

    void BuildView()
    {
        Font = SystemFonts.MessageBoxFont;
        AutoScaleDimensions = new SizeF(96, 96);
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(656, 490);
        MinimumSize = new Size(400, 300);

        view.Resources.MergedDictionaries.Add(design);
        status = (Ui.TextBlock)view.FindName("Status");
        detail = (Ui.TextBlock)view.FindName("Detail");
        connect = (Ui.Button)view.FindName("Connect");
        pause = (Ui.Button)view.FindName("Pause");
        settings = (Ui.Button)view.FindName("Arrange");
        more = (Ui.Button)view.FindName("More");
        startup = (Ui.CheckBox)view.FindName("Startup");
        standardChoice = (Ui.RadioButton)view.FindName("Standard");
        fullChoice = (Ui.RadioButton)view.FindName("Full");
        qualityHint = (Ui.TextBlock)view.FindName("QualityHint");
        signal = (Shapes.Ellipse)view.FindName("Signal");
        panelWallpaper = (Ui.Canvas)view.FindName("PanelWallpaper");

        var diagram = (Ui.Viewbox)view.FindName("Diagram");
        view.SizeChanged += delegate {
            diagram.Visibility = view.ActualWidth >= 535 ? W.Visibility.Visible : W.Visibility.Collapsed;
        };
        Controls.Add(new ElementHost { Dock = DockStyle.Fill, Child = view });
        ConfigureMoreMenu();
        standardChoice.Checked += delegate { SelectChroma(false); };
        fullChoice.Checked += delegate { SelectChroma(true); };
        CheckChroma(ChromaPreference.ReadFull());
    }

    void ShowSignal()
    {
        string brush = "Waiting";
        switch (currentStatus)
        {
            case "streaming": brush = "Online"; break;
            case "paused": case "unplugged": case "disconnecting": case "stopped": brush = "HeroMuted"; break;
            case "usb_error": case "driver_error": case "display_error": case "start_error": case "slow_usb":
            case "multiple_devices": case "other_display": case "incompatible_display": case "other_session":
            case "waiting_for_separate_landscape_monitor": case "extend_display": case "error": case "connection_ended": brush = "Attention"; break;
        }
        signal.SetResourceReference(Shapes.Shape.FillProperty, brush);
        panelWallpaper.Visibility = currentStatus == "streaming" ? W.Visibility.Visible : W.Visibility.Hidden;
    }

    protected override void OnLoad(EventArgs e)
    {
        var area = Screen.FromControl(this).WorkingArea;
        if (Width > area.Width || Height > area.Height) {
            int width = Math.Min(Width, area.Width), height = Math.Min(Height, area.Height);
            SetBounds(area.Left + (area.Width - width) / 2, area.Top + (area.Height - height) / 2, width, height);
        }
        base.OnLoad(e);
    }

    static bool DarkAppearance()
    {
        try {
            return Object.Equals(Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
                                                   "AppsUseLightTheme", 1), 0);
        }
        catch (SecurityException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
        catch (IOException) { return false; }
    }

    static Media.Color ColorValue(string hex) { return (Media.Color)Media.ColorConverter.ConvertFromString(hex); }

    void Brush(string name, string light, string dark, Media.Color contrast)
    {
        var color = previousContrast == true ? contrast : ColorValue(previousDark == true ? dark : light);
        var brush = new Media.SolidColorBrush(color);
        brush.Freeze();
        design[name] = brush;
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("user32.dll")]
    static extern uint GetDpiForWindow(IntPtr window);

    void ApplyAppearance()
    {
        SetAppearance(DarkAppearance(), SystemInformation.HighContrast);
    }

    void SetAppearance(bool dark, bool contrast)
    {
        if (previousDark == dark && previousContrast == contrast) return;
        previousDark = dark;
        previousContrast = contrast;
        var paper = W.SystemColors.WindowColor;
        var ink = W.SystemColors.WindowTextColor;
        var accent = W.SystemColors.HighlightColor;
        var selectedInk = W.SystemColors.HighlightTextColor;
        Brush("Canvas", "#F4F5F8", "#17191F", paper);
        Brush("Surface", "#FFFFFF", "#24272E", paper);
        Brush("Edge", "#E0E3E9", "#41454E", ink);
        Brush("Ink", "#1A2332", "#F4F5F8", ink);
        Brush("Muted", "#516073", "#B9C2CF", ink);
        Brush("Accent", "#265E9B", "#A5C9FF", W.SystemColors.HotTrackColor);
        Brush("Tint", "#EBF1F8", "#313C4C", paper);
        Brush("Track", "#EDF0F4", "#171B22", paper);
        Brush("Selected", "#FFFFFF", "#3B4655", accent);
        Brush("SelectedInk", "#204F87", "#E7F1FF", selectedInk);
        Brush("SelectionEdge", "#BDCADA", "#66788F", ink);
        Brush("SwitchOn", "#2366AD", "#85B9F6", accent);
        Brush("OnKnob", "#FFFFFF", "#152237", selectedInk);
        Brush("SwitchOff", "#808B99", "#798491", ink);
        Brush("Knob", "#FFFFFF", "#151A22", paper);
        Brush("Focus", "#1979CB", "#9ACBFF", accent);
        Brush("HeroInk", "#FFFFFF", "#FFFFFF", ink);
        Brush("HeroInverse", "#162E4A", "#162E4A", paper);
        Brush("HeroMuted", "#D5DFF0", "#D5DFF0", ink);
        Brush("HeroControl", "#324965", "#324965", paper);
        Brush("HeroOutline", "#6E829E", "#7588A6", ink);
        Brush("Online", "#75E3B0", "#75E3B0", ink);
        Brush("Waiting", "#9FD2FF", "#9FD2FF", ink);
        Brush("Attention", "#FFD28B", "#FFD28B", ink);
        design["Hero"] = contrast ? design["Canvas"] : new Media.LinearGradientBrush(ColorValue("#142B43"), ColorValue("#3B456B"), 12);

        var bg = System.Drawing.ColorTranslator.FromHtml(dark ? "#17191F" : "#F4F5F8");
        var surface = System.Drawing.ColorTranslator.FromHtml(dark ? "#24272E" : "#FFFFFF");
        var hover = System.Drawing.ColorTranslator.FromHtml(dark ? "#39465A" : "#DFE9F5");
        var text = System.Drawing.ColorTranslator.FromHtml(dark ? "#F4F5F8" : "#1A2332");
        BackColor = contrast ? System.Drawing.SystemColors.Window : bg;
        foreach (var menu in new[] {moreMenu, tray.ContextMenuStrip}) {
            if (contrast) menu.RenderMode = ToolStripRenderMode.System;
            else menu.Renderer = new DisplayMenuRenderer(surface, hover, text);
            menu.BackColor = contrast ? System.Drawing.SystemColors.Menu : surface;
            menu.ForeColor = contrast ? System.Drawing.SystemColors.MenuText : text;
        }
        int useDarkTitle = dark && !contrast ? 1 : 0;
        DwmSetWindowAttribute(Handle, 20, ref useDarkTitle, sizeof(int));
    }

    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);
        if ((m.Msg == 0x001A || m.Msg == 0x0015 || m.Msg == 0x031A) && tray != null && tray.ContextMenuStrip != null && !IsDisposed)
            ApplyAppearance();
    }

    sealed class DisplayMenuRenderer : ToolStripProfessionalRenderer
    {
        sealed class MenuColors : ProfessionalColorTable
        {
            readonly Color surface, hover;
            internal MenuColors(Color surface, Color hover) { this.surface = surface; this.hover = hover; }
            public override Color ToolStripDropDownBackground { get { return surface; } }
            public override Color ImageMarginGradientBegin { get { return surface; } }
            public override Color ImageMarginGradientMiddle { get { return surface; } }
            public override Color ImageMarginGradientEnd { get { return surface; } }
            public override Color MenuItemSelected { get { return hover; } }
            public override Color MenuItemSelectedGradientBegin { get { return hover; } }
            public override Color MenuItemSelectedGradientEnd { get { return hover; } }
            public override Color MenuItemPressedGradientBegin { get { return hover; } }
            public override Color MenuItemPressedGradientMiddle { get { return hover; } }
            public override Color MenuItemPressedGradientEnd { get { return hover; } }
            public override Color MenuItemBorder { get { return hover; } }
            public override Color CheckBackground { get { return hover; } }
        }
        readonly Color ink;
        internal DisplayMenuRenderer(Color surface, Color hover, Color ink) : base(new MenuColors(surface, hover)) { this.ink = ink; RoundedEdges = false; }
        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            if (e.Item.Enabled) e.TextColor = ink;
            base.OnRenderItemText(e);
        }
        protected override void OnRenderArrow(ToolStripArrowRenderEventArgs e)
        {
            if (e.Item.Enabled) e.ArrowColor = ink;
            base.OnRenderArrow(e);
        }
    }
}
