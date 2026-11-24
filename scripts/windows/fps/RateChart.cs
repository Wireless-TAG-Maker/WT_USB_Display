using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Globalization;
using System.Windows.Forms.DataVisualization.Charting;
class RateChart
{
    static Chart Make(string[] rows, string title, string[] keys, string[] names, Color[] colors, int xmax)
    {
        var c = new Chart { Width = 1000, Height = 345, BackColor = Color.White };
        c.AntiAliasing = AntiAliasingStyles.All;
        var a = new ChartArea();
        a.BackColor = Color.White;
        c.ChartAreas.Add(a);
        foreach (var axis in new[] { a.AxisX, a.AxisY })
        {
            axis.Minimum = 0;
            axis.IsMarginVisible = false;
            axis.MajorGrid.LineColor = Color.FromArgb(224, 229, 234);
            axis.LineColor = Color.FromArgb(117, 132, 143);
            axis.MajorTickMark.Enabled = false;
            axis.LabelStyle.Font = new Font("Segoe UI", 9);
            axis.TitleFont = new Font("Microsoft YaHei UI", 9);
        }
        a.AxisX.Maximum = xmax;
        a.AxisX.Interval = xmax >= 50 ? 10 : 5;
        a.AxisX.Title = "各测试开始后的时间（秒）";
        a.AxisY.Maximum = 70;
        a.AxisY.Interval = 10;
        a.AxisY.Title = "帧 / 秒";
        var legend = new Legend();
        legend.Docking = Docking.Top;
        legend.Alignment = StringAlignment.Center;
        legend.Font = new Font("Microsoft YaHei UI", 9);
        c.Legends.Add(legend);
        c.Titles.Add(new Title(title, Docking.Top, new Font("Microsoft YaHei UI", 14, FontStyle.Bold),
                               Color.FromArgb(26, 42, 55)));
        for (int i = 0; i < keys.Length; i++)
        {
            var s = new Series(names[i]);
            s.ChartType = SeriesChartType.Line;
            s.Color = colors[i];
            s.BorderWidth = 3;
            foreach (var row in rows)
            {
                var f = row.Split(',');
                if (f[0] != keys[i])
                    continue;
                s.Points.AddXY(double.Parse(f[1], CultureInfo.InvariantCulture),
                               double.Parse(f[2], CultureInfo.InvariantCulture));
            }
            c.Series.Add(s);
        }
        return c;
    }
    static void Main(string[] args)
    {
        var data = File.ReadAllLines(args[0]).Skip(1).ToArray();
        Color blue = Color.FromArgb(15, 87, 155), green = Color.FromArgb(0, 126, 105),
              gray = Color.FromArgb(105, 112, 119);
        using (var upper = Make(data, "当前桌面与固定画面", new[] { "current", "static" },
                                new[] { "当前桌面 · 呈现回执", "固定画面 · 呈现回执" }, new[] { blue, gray }, 35))
        using (var lower = Make(data, "60 Hz 连续动画", new[] { "motion", "camera" },
                                new[] { "P4 呈现回执", "实体屏可读出的不同帧（实拍下限）" }, new[] { blue, green }, 60))
        using (var image = new Bitmap(1000, 690))
        using (var g = Graphics.FromImage(image))
        {
            g.Clear(Color.White);
            foreach (var pair in new[] { Tuple.Create(upper, 0), Tuple.Create(lower, 345) })
                using (var bmp = new Bitmap(1000, 345))
                {
                    pair.Item1.DrawToBitmap(bmp, new Rectangle(0, 0, 1000, 345));
                    g.DrawImageUnscaled(bmp, 0, pair.Item2);
                }
            image.Save(args[1], System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
