// The crest taken apart, so its stones can light one by one: the painted crest (assets/triforce.png) is the only picture in
// Setup, and these layers are cut from it at start-up rather than drawn. A stone is found by its colour inside its own corner of
// the crest (the gold and the dark ground have other hues), its mask closed, filled, grown a little and feathered.
//   Dim       the crest with the three stones drained of colour and darkened (unlit)
//   Stones[i] the stone's own pixels (over Dim at full opacity they give the painting back): 0 emerald, 1 sapphire, 2 ruby
using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace TriforceSetup;

static class Art
{
    public const int Emerald = 0, Sapphire = 1, Ruby = 2;

    public static BitmapSource Crest = null!, Dim = null!, Shine = null!;
    public static readonly BitmapSource[] Stones = new BitmapSource[3];
    /// Where each stone sits in the crest (0..1 of its box), and the light it throws
    public static readonly Point[] Centre = new Point[3];
    public static readonly uint[] Glow = { 0x7CFF9A, 0x8FC0FF, 0xFF7A84 };

    // (hue window in degrees, least saturation, least value, and the stone's corner of the picture as fractions of it)
    static readonly (double h0, double h1, double sat, double val, Rect box)[] Find =
    {
        (75, 175, 0.30, 0.10, new Rect(0.371, 0.171, 0.293, 0.310)),
        (190, 265, 0.30, 0.10, new Rect(0.518, 0.481, 0.381, 0.395)),
        (332, 18, 0.40, 0.12, new Rect(0.156, 0.481, 0.283, 0.310)),
    };

    public static void Load()
    {
        var source = new FormatConvertedBitmap(Program.Image("triforce.png"), PixelFormats.Bgra32, null, 0);
        int w = source.PixelWidth, h = source.PixelHeight, stride = w * 4;
        var px = new byte[stride * h];
        source.CopyPixels(px, stride, 0);
        Crest = Make(px, w, h);

        var dim = (byte[])px.Clone();
        var gems = new float[w * h];   // (how much of each pixel is stone, any of the three)
        for (var s = 0; s < 3; s++)
        {
            var mask = Mask(px, w, h, Find[s]);
            var lit = (byte[])px.Clone();
            double sx = 0, sy = 0, sum = 0;
            for (int y = 0, i = 0; y < h; y++)
                for (var x = 0; x < w; x++, i++)
                {
                    var m = mask[i];
                    var o = i * 4;
                    lit[o + 3] = (byte)(px[o + 3] * m + 0.5f);
                    if (m <= 0) continue;
                    gems[i] = Math.Max(gems[i], m);
                    sx += x * m; sy += y * m; sum += m;
                    // (unlit: the stone's own light and dark, as cold dark glass)
                    var lum = (0.114f * px[o] + 0.587f * px[o + 1] + 0.299f * px[o + 2]) / 255f;
                    dim[o] = (byte)(dim[o] * (1 - m) + (lum * 0.26f + 0.016f) * 255f * m + 0.5f);
                    dim[o + 1] = (byte)(dim[o + 1] * (1 - m) + (lum * 0.28f + 0.018f) * 255f * m + 0.5f);
                    dim[o + 2] = (byte)(dim[o + 2] * (1 - m) + (lum * 0.30f + 0.020f) * 255f * m + 0.5f);
                }
            Stones[s] = Make(lit, w, h);
            Centre[s] = sum > 0 ? new Point(sx / sum / w, sy / sum / h) : new Point(0.5, 0.5);
        }
        Dim = Make(dim, w, h);

        // (where the gleam may show: the crest's own pixels and no others, and of those the bright gold most. Light glances
        // off polished metal; over the dark ground between it would be a grey wash, and over the stones it would bleach
        // their colour, so both get little of it)
        var shine = new byte[px.Length];
        for (var i = 0; i < w * h; i++)
        {
            var o = i * 4;
            var lum = (0.114f * px[o] + 0.587f * px[o + 1] + 0.299f * px[o + 2]) / 255f;
            var bright = Math.Max(0f, Math.Min(1f, (lum - 0.30f) / 0.50f));
            shine[o] = shine[o + 1] = shine[o + 2] = 255;
            shine[o + 3] = (byte)(px[o + 3] * (0.04f + 0.96f * bright * bright) * (1 - 0.8f * gems[i]) + 0.5f);
        }
        Shine = Make(shine, w, h);
    }

    static BitmapSource Make(byte[] bgra, int w, int h)
    {
        var b = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, bgra, w * 4);
        b.Freeze();
        return b;
    }

    static float[] Mask(byte[] px, int w, int h, (double h0, double h1, double sat, double val, Rect box) f)
    {
        var on = new bool[w * h];
        int x0 = (int)(f.box.X * w), y0 = (int)(f.box.Y * h), x1 = (int)(f.box.Right * w), y1 = (int)(f.box.Bottom * h);
        for (var y = y0; y < y1; y++)
            for (var x = x0; x < x1; x++)
            {
                var o = (y * w + x) * 4;
                if (px[o + 3] < 128) continue;
                double b = px[o] / 255.0, g = px[o + 1] / 255.0, r = px[o + 2] / 255.0;
                double max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b)), d = max - min;
                if (max < f.val || d < 1e-6 || d / max < f.sat) continue;
                var hue = (max == r ? ((g - b) / d + 6) % 6 : max == g ? (b - r) / d + 2 : (r - g) / d + 4) * 60;
                on[y * w + x] = f.h0 < f.h1 ? hue > f.h0 && hue < f.h1 : hue > f.h0 || hue < f.h1;
            }
        on = Grow(on, w, h, 2, true);
        on = Grow(on, w, h, 2, false);

        // (holes: the white sparkles inside a stone have no hue. Whatever the outside cannot reach belongs to the stone)
        var outside = new bool[w * h];
        var stack = new Stack<int>();
        stack.Push(0);
        outside[0] = true;
        while (stack.Count > 0)
        {
            var i = stack.Pop();
            int x = i % w, y = i / w;
            if (x > 0 && !outside[i - 1] && !on[i - 1]) { outside[i - 1] = true; stack.Push(i - 1); }
            if (x < w - 1 && !outside[i + 1] && !on[i + 1]) { outside[i + 1] = true; stack.Push(i + 1); }
            if (y > 0 && !outside[i - w] && !on[i - w]) { outside[i - w] = true; stack.Push(i - w); }
            if (y < h - 1 && !outside[i + w] && !on[i + w]) { outside[i + w] = true; stack.Push(i + w); }
        }
        for (var i = 0; i < on.Length; i++) on[i] = !outside[i];
        on = Grow(on, w, h, 1, true);

        var m = new float[w * h];
        for (var i = 0; i < m.Length; i++) m[i] = on[i] ? 1 : 0;
        for (var pass = 0; pass < 2; pass++) m = Soften(m, w, h);
        return m;
    }

    /// The mask grown (or shrunk) by r pixels
    static bool[] Grow(bool[] a, int w, int h, int r, bool grow)
    {
        var o = new bool[a.Length];
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++)
            {
                var hit = !grow;
                for (var dy = -r; dy <= r && hit != grow; dy++)
                    for (var dx = -r; dx <= r; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        var v = xx >= 0 && yy >= 0 && xx < w && yy < h && a[yy * w + xx];
                        if (v == grow) { hit = grow; break; }
                    }
                o[y * w + x] = hit;
            }
        return o;
    }

    /// A 1-2-1 blur, across then down
    static float[] Soften(float[] a, int w, int h)
    {
        var t = new float[a.Length];
        var o = new float[a.Length];
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++)
            {
                var i = y * w + x;
                t[i] = (a[i] * 2 + a[x > 0 ? i - 1 : i] + a[x < w - 1 ? i + 1 : i]) / 4;
            }
        for (var y = 0; y < h; y++)
            for (var x = 0; x < w; x++)
            {
                var i = y * w + x;
                o[i] = (t[i] * 2 + t[y > 0 ? i - w : i] + t[y < h - 1 ? i + w : i]) / 4;
            }
        return o;
    }

    /// Film grain for the hall: fine light and dark specks, tiled, at a few percent. It keeps the soft gradients from banding.
    public static ImageBrush Grain()
    {
        const int n = 160;
        var px = new byte[n * n * 4];
        var r = new Random(11);
        for (var i = 0; i < n * n; i++)
        {
            var v = r.NextDouble();
            var light = r.Next(2) == 0;
            var a = (byte)(v * v * 26);
            // (premultiplied: white or black at a small alpha)
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = light ? a : (byte)0;
            px[i * 4 + 3] = a;
        }
        var b = BitmapSource.Create(n, n, 96, 96, PixelFormats.Pbgra32, null, px, n * 4);
        b.Freeze();
        var brush = new ImageBrush(b) { TileMode = TileMode.Tile, Viewport = new Rect(0, 0, n, n), ViewportUnits = BrushMappingMode.Absolute, Stretch = Stretch.None };
        brush.Freeze();
        return brush;
    }

    /// The lattice in the light behind the crest: equilateral triangles, hairlines of gold
    public static DrawingBrush Lattice(double side, Color line)
    {
        var rise = side * Math.Sqrt(3) / 2;
        var g = new StreamGeometry();
        using (var c = g.Open())
        {
            void L(double ax, double ay, double bx, double by) { c.BeginFigure(new Point(ax, ay), false, false); c.LineTo(new Point(bx, by), true, false); }
            L(0, 0, side, 0);
            L(0, rise, side, rise);
            L(0, 0, side / 2, rise); L(side / 2, rise, side, 0);
            L(0, rise * 2, side / 2, rise); L(side / 2, rise, side, rise * 2);
        }
        g.Freeze();
        var pen = new Pen(new SolidColorBrush(line), 1);
        pen.Freeze();
        var d = new DrawingGroup();
        d.Children.Add(new GeometryDrawing(Brushes.Transparent, null, new RectangleGeometry(new Rect(0, 0, side, rise * 2))));
        d.Children.Add(new GeometryDrawing(null, pen, g));
        d.ClipGeometry = new RectangleGeometry(new Rect(0, 0, side, rise * 2));
        var brush = new DrawingBrush(d) { TileMode = TileMode.Tile, Viewport = new Rect(0, 0, side, rise * 2), ViewportUnits = BrushMappingMode.Absolute, Stretch = Stretch.None };
        brush.Freeze();
        return brush;
    }
}
