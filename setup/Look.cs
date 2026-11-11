// How Triforce Setup looks and moves, in small pieces the window (Main.xaml) and the pages use: letter-spaced capitals, the
// progress line, the motes of light, and the animation helpers. The values are the style tokens (palette, type, motion) that
// the shader screen in the game window follows too.
using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace TriforceSetup;

public static class Fx
{
    /// --snapshot: pictures of the pages, so nothing moves and everything is at its resting value
    public static bool Still;
    /// The window is minimized: nothing is seen, so the motes and the progress lines stop redrawing
    public static bool Paused;

    public const int Fast = 120, Base = 220, Slow = 420;
    public static readonly IEasingFunction Out = Frozen(new QuinticEase { EasingMode = EasingMode.EaseOut });
    public static readonly IEasingFunction InOut = Frozen(new SineEase { EasingMode = EasingMode.EaseInOut });

    static T Frozen<T>(T f) where T : Freezable { f.Freeze(); return f; }

    public static Color Rgb(uint rgb, double a = 1) => Color.FromArgb((byte)Math.Round(a * 255), (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
    public static SolidColorBrush Paint(uint rgb, double a = 1) => Frozen(new SolidColorBrush(Rgb(rgb, a)));

    // (the palette: style_tokens.md section 2)
    public const uint Ink900 = 0x0A0908, Ink800 = 0x12100C, Ink700 = 0x1B1711, Ink600 = 0x262017, Line = 0x2C2518,
                      TextHi = 0xF3EAD5, Text = 0xCDC2A8, TextDim = 0x948A70, TextFaint = 0x5F5744,
                      Gold100 = 0xFFF4CC, Gold300 = 0xF7DC8C, Gold500 = 0xE0B23E, Gold600 = 0xC4912A, Gold700 = 0x9A6A18, Gold900 = 0x3D2A08,
                      Good = 0x6BE07F, Bad = 0xFF6B76;

    /// A value eased to its new place (or put there at once for a still picture)
    public static void To(UIElement e, DependencyProperty p, double to, int ms, int delay = 0, IEasingFunction? ease = null)
    {
        if (Still || ms <= 0) { e.BeginAnimation(p, null); e.SetValue(p, to); return; }
        e.BeginAnimation(p, new DoubleAnimation(to, TimeSpan.FromMilliseconds(ms)) { EasingFunction = ease ?? Out, BeginTime = TimeSpan.FromMilliseconds(delay) });
    }

    public static void To(Animatable e, DependencyProperty p, double to, int ms, int delay = 0, IEasingFunction? ease = null)
    {
        if (Still || ms <= 0) { e.BeginAnimation(p, null); e.SetValue(p, to); return; }
        e.BeginAnimation(p, new DoubleAnimation(to, TimeSpan.FromMilliseconds(ms)) { EasingFunction = ease ?? Out, BeginTime = TimeSpan.FromMilliseconds(delay) });
    }

    /// From one value to another, once (a bloom, a gleam)
    public static void Once(UIElement e, DependencyProperty p, double from, double to, int ms, int delay = 0, IEasingFunction? ease = null)
    {
        if (Still) { e.BeginAnimation(p, null); e.SetValue(p, to); return; }
        e.BeginAnimation(p, new DoubleAnimation(from, to, TimeSpan.FromMilliseconds(ms)) { EasingFunction = ease ?? Out, BeginTime = TimeSpan.FromMilliseconds(delay) });
    }

    public static void Once(Animatable e, DependencyProperty p, double from, double to, int ms, int delay = 0, IEasingFunction? ease = null)
    {
        if (Still) { e.BeginAnimation(p, null); e.SetValue(p, to); return; }
        e.BeginAnimation(p, new DoubleAnimation(from, to, TimeSpan.FromMilliseconds(ms)) { EasingFunction = ease ?? Out, BeginTime = TimeSpan.FromMilliseconds(delay) });
    }

    /// Breathing between two values for as long as the window lives (a still picture rests at the higher one)
    public static void Breathe(UIElement e, DependencyProperty p, double low, double high, int cycleMs, int offsetMs = 0)
    {
        if (Still) { e.BeginAnimation(p, null); e.SetValue(p, high); return; }
        var a = new DoubleAnimation(low, high, TimeSpan.FromMilliseconds(cycleMs / 2.0)) { EasingFunction = InOut, AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever };
        var clock = a.CreateClock();
        e.ApplyAnimationClock(p, clock);
        if (offsetMs > 0) clock.Controller?.Seek(TimeSpan.FromMilliseconds(offsetMs), TimeSeekOrigin.BeginTime);
    }

    /// Letter-spacing, which WPF's text has none of: hair spaces between the letters (whole text elements, so accents stay put)
    public static string Track(string s, int gaps)
    {
        if (gaps <= 0 || s.Length < 2) return s;
        var gap = new string(' ', gaps);
        var sb = new StringBuilder(s.Length * (gaps + 1));
        var e = StringInfo.GetTextElementEnumerator(s);
        var first = true;
        while (e.MoveNext())
        {
            if (!first) sb.Append(gap);
            sb.Append(e.GetTextElement());
            first = false;
        }
        return sb.ToString();
    }

    /// A page arriving: it fades in while it rises, its blocks a moment apart
    public static void Arrive(Panel page)
    {
        var n = 0;
        foreach (UIElement child in page.Children)
        {
            if (child.Visibility != Visibility.Visible && !Still) { child.BeginAnimation(UIElement.OpacityProperty, null); child.Opacity = 1; continue; }
            var move = child.RenderTransform as TranslateTransform;
            if (move == null || move.IsFrozen) child.RenderTransform = move = new TranslateTransform();
            var delay = Math.Min(n++, 6) * 45;
            Once(child, UIElement.OpacityProperty, 0, 1, 320, delay);
            Once(move, TranslateTransform.YProperty, 12, 0, Slow, delay);
        }
    }
}

/// A label in capitals with letter-spacing: Label is the plain text, Gaps the hair spaces between letters
public sealed class Caps : TextBlock
{
    public static readonly DependencyProperty LabelProperty = DependencyProperty.Register(nameof(Label), typeof(string), typeof(Caps),
        new PropertyMetadata("", (d, _) => ((Caps)d).Apply()));
    public static readonly DependencyProperty GapsProperty = DependencyProperty.Register(nameof(Gaps), typeof(int), typeof(Caps),
        new PropertyMetadata(2, (d, _) => ((Caps)d).Apply()));

    public string Label { get => (string)GetValue(LabelProperty); set => SetValue(LabelProperty, value); }
    public int Gaps { get => (int)GetValue(GapsProperty); set => SetValue(GapsProperty, value); }

    void Apply() => Text = Fx.Track((Label ?? "").ToUpper(CultureInfo.CurrentUICulture), Gaps);
}

/// The progress line: a fine gold line with a spark at its head and a glint that runs along it. Value 0..1 eases to its new
/// place; IsIndeterminate sweeps a piece of light along the track while the length of the work is unknown.
public sealed class ProgressLine : FrameworkElement
{
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(nameof(Value), typeof(double), typeof(ProgressLine),
        new FrameworkPropertyMetadata(0.0, (d, _) => ((ProgressLine)d).Wake()));
    public static readonly DependencyProperty IsIndeterminateProperty = DependencyProperty.Register(nameof(IsIndeterminate), typeof(bool), typeof(ProgressLine),
        new FrameworkPropertyMetadata(false, (d, _) => ((ProgressLine)d).Wake()));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public bool IsIndeterminate { get => (bool)GetValue(IsIndeterminateProperty); set => SetValue(IsIndeterminateProperty, value); }

    static readonly Brush Track = Fx.Paint(0x2E2719);
    static readonly Brush Spark = Fx.Paint(Fx.Gold100);
    static readonly Brush Fill = Gradient(0, (Fx.Rgb(Fx.Gold600), 0), (Fx.Rgb(Fx.Gold500), 0.6), (Fx.Rgb(Fx.Gold300), 1));
    static readonly Brush Glow = Gradient(90, (Fx.Rgb(Fx.Gold500, 0), 0), (Fx.Rgb(Fx.Gold500, 0.30), 0.5), (Fx.Rgb(Fx.Gold500, 0), 1));
    static readonly Brush Glint = Gradient(0, (Fx.Rgb(Fx.Gold100, 0), 0), (Fx.Rgb(Fx.Gold100, 0.95), 0.5), (Fx.Rgb(Fx.Gold100, 0), 1));
    static readonly Brush Sweep = Gradient(0, (Fx.Rgb(Fx.Gold500, 0), 0), (Fx.Rgb(Fx.Gold300), 0.5), (Fx.Rgb(Fx.Gold500, 0), 1));
    static readonly Brush Halo = Radial((Fx.Rgb(Fx.Gold300, 0.60), 0), (Fx.Rgb(Fx.Gold500, 0.18), 0.5), (Fx.Rgb(Fx.Gold500, 0), 1));

    static Brush Gradient(double angle, params (Color c, double at)[] stops)
    {
        var b = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = angle == 0 ? new Point(1, 0) : new Point(0, 1) };
        foreach (var (c, at) in stops) b.GradientStops.Add(new GradientStop(c, at));
        b.Freeze();
        return b;
    }

    static Brush Radial(params (Color c, double at)[] stops)
    {
        var b = new RadialGradientBrush();
        foreach (var (c, at) in stops) b.GradientStops.Add(new GradientStop(c, at));
        b.Freeze();
        return b;
    }

    readonly Stopwatch clock = Stopwatch.StartNew();
    double shown, last;
    bool ticking;

    public ProgressLine()
    {
        Height = 16;
        SnapsToDevicePixels = true;
        IsVisibleChanged += (_, _) => Wake();
        Unloaded += (_, _) => Sleep();
    }

    void Wake()
    {
        if (Fx.Still) { shown = Value; InvalidateVisual(); return; }
        if (IsVisible && !ticking) { ticking = true; last = clock.Elapsed.TotalSeconds; CompositionTarget.Rendering += Tick; }
        InvalidateVisual();
    }

    void Sleep()
    {
        if (ticking) { ticking = false; CompositionTarget.Rendering -= Tick; }
    }

    void Tick(object? sender, EventArgs e)
    {
        if (!IsVisible) { Sleep(); return; }
        if (Fx.Paused) return;
        var now = clock.Elapsed.TotalSeconds;
        var dt = Math.Min(0.1, now - last);
        last = now;
        var target = Math.Max(0, Math.Min(1, Value));
        // (eases to a new value in about 300 ms; a drop back to the start is taken at once)
        shown = target < shown ? target : shown + (target - shown) * Math.Min(1, dt * 11);
        if (Math.Abs(target - shown) < 0.0005) shown = target;
        // (nothing left to move: a full or empty line at rest)
        if (!IsIndeterminate && shown == target && (target <= 0 || target >= 1)) { Sleep(); }
        InvalidateVisual();
    }

    protected override Size MeasureOverride(Size available) => new(double.IsInfinity(available.Width) ? 240 : available.Width, 16);

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth;
        if (w < 4) return;
        const double mid = 8;
        var t = Fx.Still ? 1.35 : clock.Elapsed.TotalSeconds;
        dc.DrawRectangle(Track, null, new Rect(0, mid - 1, w, 2));
        if (IsIndeterminate)
        {
            var u = Fx.Still ? 0.42 : t / 1.6 % 1.0;
            u = 0.5 - 0.5 * Math.Cos(u * Math.PI);   // (ease in and out)
            var len = w * 0.28;
            var x = -len + (w + len) * u;
            dc.PushClip(new RectangleGeometry(new Rect(0, 0, w, 16)));
            dc.DrawRectangle(Glow, null, new Rect(x, mid - 6, len, 12));
            dc.DrawRectangle(Sweep, null, new Rect(x, mid - 1, len, 2));
            dc.Pop();
            return;
        }
        var head = w * Math.Max(0, Math.Min(1, shown));
        if (head < 0.5) return;
        dc.DrawRectangle(Glow, null, new Rect(0, mid - 6, head, 12));
        dc.DrawRectangle(Fill, null, new Rect(0, mid - 1, head, 2));
        if (shown < 1)
        {
            // (the glint, along the part that is filled)
            const double glint = 70;
            var g = -glint + (head + glint) * (t / 2.4 % 1.0);
            dc.PushClip(new RectangleGeometry(new Rect(0, 0, head, 16)));
            dc.DrawRectangle(Glint, null, new Rect(g, mid - 1.5, glint, 3));
            dc.Pop();
            // (the spark at the head: a small diamond in its own light)
            dc.DrawEllipse(Halo, null, new Point(head, mid), 11, 11);
            dc.PushTransform(new RotateTransform(45, head, mid));
            dc.DrawRectangle(Spark, null, new Rect(head - 2.6, mid - 2.6, 5.2, 5.2));
            dc.Pop();
        }
    }
}

/// Motes of light drifting up through the hall: a few gold specks, slow, never the same twice. Light, not objects.
public sealed class Motes : FrameworkElement
{
    struct Mote { public double X, Y, Rise, Sway, Turn, Size, Alpha, Period, Offset; }

    readonly Mote[] motes;
    readonly Stopwatch clock = Stopwatch.StartNew();
    static readonly Brush Light = MakeLight();
    bool ticking;
    int frame;

    static Brush MakeLight()
    {
        var b = new RadialGradientBrush();
        b.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold100, 1), 0));
        b.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold300, 0.55), 0.35));
        b.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold500, 0), 1));
        b.Freeze();
        return b;
    }

    public Motes(int count = 26, int seed = 7)
    {
        IsHitTestVisible = false;
        var r = new Random(seed);
        motes = new Mote[count];
        for (var i = 0; i < count; i++)
            motes[i] = new Mote
            {
                X = r.NextDouble(), Y = 0.25 + r.NextDouble() * 0.75, Rise = 0.18 + r.NextDouble() * 0.30, Sway = 6 + r.NextDouble() * 16,
                Turn = 0.6 + r.NextDouble() * 1.4, Size = 1.6 + r.NextDouble() * 3.2, Alpha = 0.16 + r.NextDouble() * 0.42,
                Period = 11 + r.NextDouble() * 12, Offset = r.NextDouble() * 30,
            };
        IsVisibleChanged += (_, _) => Wake();
        Unloaded += (_, _) => Sleep();
    }

    void Wake()
    {
        if (Fx.Still || !IsVisible || ticking) return;
        ticking = true;
        CompositionTarget.Rendering += Tick;
    }

    void Sleep()
    {
        if (ticking) { ticking = false; CompositionTarget.Rendering -= Tick; }
    }

    void Tick(object? sender, EventArgs e)
    {
        if (!IsVisible) { Sleep(); return; }
        if (!Fx.Paused && (frame++ & 1) == 0) InvalidateVisual();   // (every other frame: they are slow)
    }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w < 10 || h < 10) return;
        var t = Fx.Still ? 20.0 : clock.Elapsed.TotalSeconds;
        foreach (var m in motes)
        {
            var u = (t + m.Offset) / m.Period % 1.0;
            var fade = Math.Min(1, u / 0.18) * Math.Min(1, (1 - u) / 0.30);   // (born softly, gone softly)
            var x = m.X * w + Math.Sin(u * Math.PI * 2 * m.Turn + m.Offset) * m.Sway;
            var y = (m.Y - u * m.Rise) * h;
            dc.PushOpacity(m.Alpha * fade);
            dc.DrawEllipse(Light, null, new Point(x, y), m.Size, m.Size);
            dc.Pop();
        }
    }
}
