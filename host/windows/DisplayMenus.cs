using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Security;
using System.Windows.Forms;
using Microsoft.Win32;
using W = System.Windows;
using Ui = System.Windows.Controls;
using System.Windows.Documents;
using Input = System.Windows.Input;
using WindowInteropHelper = System.Windows.Interop.WindowInteropHelper;
using System.Windows.Media.Imaging;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]
[assembly: System.Reflection.AssemblyTitle(DisplayApp.EnglishProductName)]
[assembly: System.Reflection.AssemblyProduct(DisplayApp.EnglishProductName)]
[assembly: System.Reflection.AssemblyCompany(DisplayApp.Publisher)]
[assembly: System.Reflection.AssemblyCopyright(DisplayApp.CopyrightNotice)]
[assembly: System.Reflection.AssemblyVersion(DisplayApp.ReleaseVersion)]
[assembly: System.Reflection.AssemblyInformationalVersion(DisplayApp.ReleaseVersion)]

sealed partial class DisplayApp
{
    internal const string ReleaseVersion = "0.4.3";
    const string Brand = "Wireless-tag";
    internal const string EnglishProductName = Brand + " USB Display";
    const string ChineseProductName = Brand + " USB 副屏";
    internal const string Publisher = Brand + " Technology Co., Ltd.";
    const string ChinesePublisher = "深圳市启明云端科技有限公司";
    const string CopyrightPrefix = "Copyright © 2026 ";
    internal const string CopyrightNotice = CopyrightPrefix + Publisher + " All rights reserved.";
    const string WebsiteUrl = "https://www.wireless-tag.com/";
    const string DocsUrl = "https://wiki.wireless-tag.com/docs/zh/";
    const string ProjectUrl = "https://github.com/Wireless-TAG-Maker/WT_USB_Display";
    const string ReleasesUrl = ProjectUrl + "/releases";
    readonly ContextMenuStrip moreMenu = new ContextMenuStrip { ShowImageMargin = false };
    readonly ToolStripMenuItem exitItem = new ToolStripMenuItem();
    readonly ToolStripMenuItem languageMenu = new ToolStripMenuItem();
    readonly ToolStripMenuItem chineseItem = new ToolStripMenuItem("简体中文"), englishItem = new ToolStripMenuItem("English");
    readonly ToolStripMenuItem helpMenu = new ToolStripMenuItem(), docsItem = new ToolStripMenuItem();
    readonly ToolStripMenuItem updatesItem = new ToolStripMenuItem(), aboutItem = new ToolStripMenuItem();
    bool english;
    bool fullChromaSelected;

    string T(string chinese, string en) { return english ? en : chinese; }
    string Product { get { return T(ChineseProductName, EnglishProductName); } }

    static bool ReadLanguage()
    {
        try
        {
            var saved = Registry.GetValue(ChromaPreference.SettingsKey, "Language", null) as string;
            if (saved == "en" || saved == "zh-CN") return saved == "en";
        }
        catch (SecurityException) { }
        catch (UnauthorizedAccessException) { }
        catch (IOException) { }
        return CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "zh";
    }

    void SelectLanguage(bool useEnglish)
    {
        english = useEnglish;
        ApplyLanguage();
        try
        {
            Registry.SetValue(ChromaPreference.SettingsKey, "Language", english ? "en" : "zh-CN");
            return;
        }
        catch (SecurityException) { }
        catch (UnauthorizedAccessException) { }
        catch (IOException) { }
        MessageBox.Show(this, T("本次已切换语言，但无法保存选择。", "The language changed for this session, but the preference could not be saved."),
            Product, MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    void CheckChroma(bool full)
    {
        fullChromaSelected = full;
        standardChoice.IsChecked = !full;
        fullChoice.IsChecked = full;
        qualityHint.Text = full ? T("完整色度，保留彩色文字细节。", "Full color detail for fine text.")
                                : T("标准色度，节省 USB 带宽。", "Standard color uses less USB bandwidth.");
    }

    void SelectChroma(bool full)
    {
        if (full == fullChromaSelected) return;
        try
        {
            ChromaPreference.Save(full);
            CheckChroma(full);
            return;
        }
        catch (SecurityException) { }
        catch (UnauthorizedAccessException) { }
        catch (IOException) { }
        CheckChroma(fullChromaSelected);
        MessageBox.Show(this, T("无法保存色度设置。", "The chroma setting could not be saved."),
            Product, MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    void ApplyLanguage()
    {
        SuspendLayout();
        Text = Product;
        exitItem.Text = T("退出(&X)", "E&xit");
        languageMenu.Text = T("语言 / &Language", "&Language / 语言");
        chineseItem.Checked = !english;
        englishItem.Checked = english;
        helpMenu.Text = T("帮助(&H)", "&Help");
        docsItem.Text = T("使用文档(&D)", "&Documentation (Chinese)");
        updatesItem.Text = T("获取更新(&U)…", "Get &updates…");
        aboutItem.Text = T("关于 " + Product + "(&A)…", "&About " + Product + "…");
        design["ConnectionLabel"] = T("USB 扩展桌面", "USB extended desktop");
        design["SettingsTitle"] = T("显示设置", "Display settings");
        moreMenu.AccessibleName = T("更多选项", "More options");
        design["MoreLabel"] = T("更多", "More");
        design["MoreHint"] = T("语言、帮助和退出（F10）", "Language, help and exit (F10)");
        design["ArrangeTitle"] = T("显示器排列", "Arrange displays");
        design["ArrangeHint"] = T("在 Windows 中调整屏幕位置和分辨率。", "Change screen positions and resolution in Windows.");
        design["QualityTitle"] = T("色度采样", "Color sampling");
        design["StandardName"] = T("标准色度 4:2:0", "Standard chroma 4:2:0");
        design["FullName"] = T("完整色度 4:4:4", "Full chroma 4:4:4");
        design["StartupTitle"] = T("登录 Windows 时自动启动", "Start at sign-in");
        design["StartupHint"] = T("在通知区域等待副屏连接。", "Wait for the display in the notification area.");
        CheckChroma(fullChromaSelected);
        trayOpen.Text = T("打开 ", "Open ") + Product;
        RenderStatus();
        ResumeLayout(true);
    }

    void ConfigureMoreMenu()
    {
        exitItem.Click += delegate { RequestExit(); };
        chineseItem.Click += delegate { SelectLanguage(false); };
        englishItem.Click += delegate { SelectLanguage(true); };
        languageMenu.DropDownItems.AddRange(new ToolStripItem[] { chineseItem, englishItem });
        docsItem.Click += delegate { OpenLocation(this, DocsUrl); };
        updatesItem.Click += delegate { OpenLocation(this, ReleasesUrl); };
        aboutItem.Click += delegate { ShowAbout(); };
        helpMenu.DropDownItems.AddRange(new ToolStripItem[] { docsItem, updatesItem, new ToolStripSeparator(), aboutItem });
        moreMenu.Items.AddRange(new ToolStripItem[] { languageMenu, helpMenu, new ToolStripSeparator(), exitItem });
        foreach (var item in new[] {languageMenu, helpMenu, exitItem})
            item.Padding = new Padding(8, 5, 8, 5);
        more.Click += delegate { OpenMoreMenu(); };
        moreMenu.Closed += delegate(object sender, ToolStripDropDownClosedEventArgs e) {
            if (e.CloseReason == ToolStripDropDownCloseReason.Keyboard && !exiting) more.Focus();
        };
        view.PreviewKeyDown += delegate(object sender, Input.KeyEventArgs e) {
            var key = e.Key == Input.Key.System ? e.SystemKey : e.Key;
            if (key == Input.Key.F10 && Input.Keyboard.Modifiers == Input.ModifierKeys.None) {
                e.Handled = true;
                OpenMoreMenu();
            }
        };
    }

    void OpenMoreMenu()
    {
        if (moreMenu.Visible) return;
        more.Focus();
        more.BringIntoView();
        view.UpdateLayout();
        var anchor = more.PointToScreen(new W.Point(more.ActualWidth, more.ActualHeight));
        var point = new System.Drawing.Point((int)Math.Round(anchor.X), (int)Math.Round(anchor.Y));
        moreMenu.Show(this, PointToClient(point), ToolStripDropDownDirection.BelowLeft);
        moreMenu.Items[0].Select();
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (keyData == Keys.F10 && !moreMenu.Visible) { OpenMoreMenu(); return true; }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    void OpenLocation(IWin32Window owner, string target)
    {
        try { using (Process.Start(new ProcessStartInfo(target) { UseShellExecute = true })) { } }
        catch (Win32Exception ex)
        {
            MessageBox.Show(owner, T("Windows 无法打开此地址。", "Windows could not open this address.") + "\n" + target + "\n" + ex.Message,
                Product, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    void AddAboutLink(Ui.StackPanel links, string caption, string url)
    {
        if (links.Children.Count > 0) {
            var line = new W.Shapes.Rectangle { Height = 1 };
            line.SetResourceReference(W.Shapes.Shape.FillProperty, "Edge");
            links.Children.Add(line);
        }
        var row = new Ui.StackPanel { Margin = new W.Thickness(0, 9, 0, 10) };
        var label = new Ui.TextBlock { Text = caption };
        label.SetResourceReference(W.FrameworkElement.StyleProperty, "RowTitle");
        row.Children.Add(label);
        var link = new Hyperlink(new Run(url));
        link.SetResourceReference(TextElement.ForegroundProperty, "Accent");
        W.Automation.AutomationProperties.SetName(link, caption + " · " + url);
        link.Click += delegate { OpenLocation(this, url); };
        var address = new Ui.TextBlock { TextWrapping = W.TextWrapping.Wrap, FontSize = 13, Margin = new W.Thickness(0, 2, 0, 0) };
        address.Inlines.Add(link);
        row.Children.Add(address);
        links.Children.Add(row);
    }

    void ShowAbout()
    {
        var dialog = ReadView<W.Window>("DisplayAbout.xaml");
        dialog.Resources.MergedDictionaries.Add(design);
        dialog.Title = T("关于 ", "About ") + Product;
        new WindowInteropHelper(dialog).Owner = Handle;
        var area = Screen.FromHandle(Handle).WorkingArea;
        double scale = GetDpiForWindow(Handle) / 96.0;
        dialog.MaxHeight = area.Height / scale;
        dialog.Width = Math.Min(dialog.Width, area.Width / scale);
        using (var stream = typeof(DisplayApp).Assembly.GetManifestResourceStream("display.ico")) {
            var frames = new IconBitmapDecoder(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad).Frames;
            var image = frames[0];
            foreach (var frame in frames) if (frame.PixelWidth > image.PixelWidth) image = frame;
            dialog.Icon = image;
            ((Ui.Image)dialog.FindName("AppIcon")).Source = image;
        }
        ((Ui.TextBlock)dialog.FindName("ProductTitle")).Text = Product;
        ((Ui.TextBlock)dialog.FindName("Version")).Text = T("版本 ", "Version ") + ReleaseVersion;
        ((Run)dialog.FindName("CopyrightPrefix")).Text = CopyrightPrefix;
        ((Run)dialog.FindName("CompanyName")).Text = T(ChinesePublisher, Publisher);
        ((Run)dialog.FindName("CopyrightSuffix")).Text = T(" 版权所有", " All rights reserved.");
        ((Hyperlink)dialog.FindName("Company")).Click += delegate {
            OpenLocation(this, T(WebsiteUrl + "p-about.html", "https://en.wireless-tag.com/p-about.html"));
        };
        var links = (Ui.StackPanel)dialog.FindName("Links");
        AddAboutLink(links, T("官方网站", "Website"), WebsiteUrl);
        AddAboutLink(links, T("中文文档", "Documentation (Chinese)"), DocsUrl);
        AddAboutLink(links, T("开源仓库", "Source repository"), ProjectUrl);
        ((Ui.Button)dialog.FindName("CloseButton")).Content = T("关闭", "Close");
        dialog.SourceInitialized += delegate {
            int dark = previousDark == true && previousContrast == false ? 1 : 0;
            DwmSetWindowAttribute(new WindowInteropHelper(dialog).Handle, 20, ref dark, sizeof(int));
        };
        dialog.ShowDialog();
    }
}
