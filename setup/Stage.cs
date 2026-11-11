// The hall and the relic: everything behind and beside the pages. A warm dark room with one soft light, and in it the crest,
// whose three stones are lit or unlit as the work goes (gathering, building) and all alight when there is nothing to wait for.
// Built in code into Main.xaml's StageHost, at fixed places: the window is one composed picture (Shell fits it to small screens).
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Shapes;

namespace TriforceSetup;

sealed class Stage
{
    public const double WindowWidth = 1060, WindowHeight = 640, Column = 430;
    const double CrestWidth = 300, CrestTop = 150;
    static readonly double CrestHeight = CrestWidth * 468 / 512;
    static readonly double MidX = WindowWidth - Column / 2, MidY = CrestTop + CrestHeight / 2;
    // (the gleam's band: its size, its lean, and how far left and right it must go to be wholly off the crest's box)
    const double BandWidth = CrestWidth * 0.24, BandLean = 24;
    static readonly double BandHeight = CrestHeight * 1.8;
    static readonly double BandReach = (BandWidth * Math.Cos(BandLean * Math.PI / 180) + BandHeight * Math.Sin(BandLean * Math.PI / 180)) / 2;
    static readonly double BandFrom = -BandReach - BandWidth / 2, BandTo = CrestWidth + BandReach - BandWidth / 2;

    public readonly Canvas Root = new() { Width = WindowWidth, Height = WindowHeight, IsHitTestVisible = false, ClipToBounds = true };
    readonly Canvas relic = new() { Width = WindowWidth, Height = WindowHeight };
    readonly Image[] stones = new Image[3];
    readonly Ellipse[] rest = new Ellipse[3], bloom = new Ellipse[3];
    readonly bool[] lit = new bool[3];
    readonly Rectangle gleam;
    readonly TranslateTransform gleamMove = new();
    readonly RotateTransform marksTurn = new();
    readonly Ellipse halo;

    static RadialGradientBrush Radial(params (Color c, double at)[] stops)
    {
        var b = new RadialGradientBrush();
        foreach (var (c, at) in stops) b.GradientStops.Add(new GradientStop(c, at));
        b.Freeze();
        return b;
    }

    static T Place<T>(T e, double x, double y) where T : UIElement
    {
        Canvas.SetLeft(e, x);
        Canvas.SetTop(e, y);
        return e;
    }

    public Stage()
    {
        // ---- the hall: light from above, the halo, the lattice in it, the dark corners, grain ----
        var top = new LinearGradientBrush(Fx.Rgb(0xFFECAA, 0.045), Fx.Rgb(0xFFECAA, 0), 90);
        top.Freeze();
        Root.Children.Add(Place(new Rectangle { Width = WindowWidth, Height = WindowHeight * 0.45, Fill = top }, 0, 0));

        const double reach = CrestWidth * 1.2;
        halo = Place(new Ellipse
        {
            Width = reach * 2, Height = reach * 2,
            Fill = Radial((Fx.Rgb(Fx.Gold500, 0.24), 0), (Fx.Rgb(Fx.Gold500, 0.075), 0.45), (Fx.Rgb(Fx.Gold500, 0), 1)),
        }, MidX - reach, MidY - reach);
        Root.Children.Add(halo);
        Root.Children.Add(Place(new Rectangle
        {
            Width = reach * 2, Height = reach * 2, Fill = Art.Lattice(56, Fx.Rgb(Fx.Gold500, 0.06)),
            OpacityMask = Radial((Colors.White, 0), (Color.FromArgb(120, 255, 255, 255), 0.5), (Colors.Transparent, 0.95)),
        }, MidX - reach, MidY - reach));

        // (an arc over the crest: a fine ring and, outside it, a ring of small marks that turns too slowly to notice, like the
        // dial of an instrument. Both fade out on their way down, before they would reach the name)
        // (the fade is pinned to the rings' box in absolute units: in relative units it maps to the bounds of what is drawn, and
        // the turning ring's bounds grow and shrink with its angle, which moved the fade up and down by some 40 px as it turned)
        const double dial = CrestWidth * 0.72;
        var arc = new LinearGradientBrush { MappingMode = BrushMappingMode.Absolute, StartPoint = new Point(0, 0), EndPoint = new Point(0, dial * 2) };
        arc.GradientStops.Add(new GradientStop(Colors.White, 0));
        arc.GradientStops.Add(new GradientStop(Colors.White, 0.42));
        arc.GradientStops.Add(new GradientStop(Colors.Transparent, 0.74));
        arc.Freeze();
        var rings = Place(new Grid { Width = dial * 2, Height = dial * 2, OpacityMask = arc }, MidX - dial, MidY - dial + CrestHeight * 0.06);
        Root.Children.Add(rings);
        foreach (var (radius, alpha, marks, seconds) in new[] { (CrestWidth * 0.66, 0.17, false, 0.0), (CrestWidth * 0.705, 0.28, true, 420.0) })
        {
            var ring = new Ellipse
            {
                Width = radius * 2, Height = radius * 2, Stroke = Fx.Paint(Fx.Gold500, alpha), StrokeThickness = marks ? 3 : 1,
                RenderTransformOrigin = new Point(0.5, 0.5), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            };
            if (marks)
            {
                // (a short stroke, a long gap; 120 of them round the ring)
                var around = 2 * Math.PI * (radius - 1.5) / 3;
                ring.StrokeDashArray = new DoubleCollection { 1.0 / 3, around / 120 - 1.0 / 3 };
                var turn = marksTurn;
                ring.RenderTransform = turn;
                if (!Fx.Still) turn.BeginAnimation(RotateTransform.AngleProperty, new DoubleAnimation(0, 360, TimeSpan.FromSeconds(seconds)) { RepeatBehavior = RepeatBehavior.Forever });
            }
            rings.Children.Add(ring);
        }

        var corners = new RadialGradientBrush { Center = new Point(0.5, 0.5), GradientOrigin = new Point(0.5, 0.5), RadiusX = 0.78, RadiusY = 0.78 };
        corners.GradientStops.Add(new GradientStop(Fx.Rgb(0, 0), 0.55));
        corners.GradientStops.Add(new GradientStop(Fx.Rgb(0, 0.55), 1));
        corners.Freeze();
        Root.Children.Add(Place(new Rectangle { Width = WindowWidth, Height = WindowHeight, Fill = corners }, 0, 0));
        Root.Children.Add(Place(new Rectangle { Width = WindowWidth, Height = WindowHeight, Fill = Art.Grain(), Opacity = 0.55 }, 0, 0));

        // ---- the relic: the crest on its light, its name under it ----
        Root.Children.Add(relic);
        double left = MidX - CrestWidth / 2;
        relic.Children.Add(Place(new Ellipse
        {
            Width = CrestWidth * 0.86, Height = 30,
            Fill = Radial((Fx.Rgb(Fx.Gold500, 0.26), 0), (Fx.Rgb(Fx.Gold500, 0.08), 0.55), (Fx.Rgb(Fx.Gold500, 0), 1)),
        }, MidX - CrestWidth * 0.43, CrestTop + CrestHeight - 13));

        var crest = Place(new Grid { Width = CrestWidth, Height = CrestHeight }, left, CrestTop);
        relic.Children.Add(crest);
        Image Layer(ImageSource s) => new() { Source = s, Stretch = Stretch.Fill };
        RenderOptions.SetBitmapScalingMode(crest, BitmapScalingMode.HighQuality);
        crest.Children.Add(Layer(Art.Dim));
        for (var s = 0; s < 3; s++)
        {
            stones[s] = Layer(Art.Stones[s]);
            stones[s].Opacity = 0;
            crest.Children.Add(stones[s]);
        }
        // (the light each stone throws once lit, and the bloom when it lights)
        var lights = new Canvas { Width = CrestWidth, Height = CrestHeight };
        crest.Children.Add(lights);
        for (var s = 0; s < 3; s++)
        {
            double cx = Art.Centre[s].X * CrestWidth, cy = Art.Centre[s].Y * CrestHeight;
            const double r = CrestWidth * 0.26;
            rest[s] = Place(new Ellipse
            {
                Width = r * 2, Height = r * 2, Opacity = 0,
                Fill = Radial((Fx.Rgb(Art.Glow[s], 0.30), 0), (Fx.Rgb(Art.Glow[s], 0.09), 0.5), (Fx.Rgb(Art.Glow[s], 0), 1)),
            }, cx - r, cy - r);
            const double b = CrestWidth * 0.5;
            bloom[s] = Place(new Ellipse
            {
                Width = b * 2, Height = b * 2, Opacity = 0, RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new ScaleTransform(0.5, 0.5),
                Fill = Radial((Fx.Rgb(Art.Glow[s], 0.75), 0), (Fx.Rgb(Art.Glow[s], 0.22), 0.4), (Fx.Rgb(Art.Glow[s], 0), 1)),
            }, cx - b, cy - b);
            lights.Children.Add(rest[s]);
            lights.Children.Add(bloom[s]);
        }
        // (the gleam: a band of light that crosses the gold, kept to the crest's own shape)
        var band = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 0) };
        band.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold100, 0), 0));
        band.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold100, 0.9), 0.5));
        band.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold100, 0), 1));
        band.Freeze();
        // The layer has the crest's own box and its mask is pinned to that box in absolute units. (A mask left in relative
        // units maps to the bounds of what is drawn, here the band itself: the crest's shape was squeezed into the band and
        // travelled with it, on and off the crest. And a band taller than a Grid's cell is cut to the cell: hence a Canvas.)
        gleam = Place(new Rectangle
        {
            Width = BandWidth, Height = BandHeight, Fill = band, RenderTransformOrigin = new Point(0.5, 0.5), Opacity = 0,
            RenderTransform = new TransformGroup { Children = { new RotateTransform(BandLean), gleamMove } },
        }, 0, (CrestHeight - BandHeight) / 2);
        var shine = new Canvas
        {
            Width = CrestWidth, Height = CrestHeight, ClipToBounds = true,
            OpacityMask = new ImageBrush(Art.Shine) { Stretch = Stretch.Fill, ViewportUnits = BrushMappingMode.Absolute, Viewport = new Rect(0, 0, CrestWidth, CrestHeight) },
        };
        shine.Children.Add(gleam);
        crest.Children.Add(shine);

        // (the name: gold capitals, wide apart, and a fine rule with a diamond)
        var metal = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        metal.GradientStops.Add(new GradientStop(Fx.Rgb(0xFFF1B8), 0.12));
        metal.GradientStops.Add(new GradientStop(Fx.Rgb(0xE8B840), 0.46));
        metal.GradientStops.Add(new GradientStop(Fx.Rgb(0xB57E1C), 0.60));
        metal.GradientStops.Add(new GradientStop(Fx.Rgb(0xF3CF6A), 0.92));
        metal.Freeze();
        var name = new StackPanel { Width = Column };
        var word = new Caps
        {
            Label = "Triforce", Gaps = 5, FontFamily = new FontFamily("Palatino Linotype, Book Antiqua, Georgia"), FontWeight = FontWeights.Bold, FontSize = 25,
            Foreground = metal, HorizontalAlignment = HorizontalAlignment.Center,
            Effect = new DropShadowEffect { Color = Colors.Black, BlurRadius = 6, ShadowDepth = 2, Direction = 270, Opacity = 0.8 },
        };
        TextOptions.SetTextFormattingMode(word, TextFormattingMode.Ideal);
        name.Children.Add(word);
        var rule = new Grid { Width = 220, Height = 12, Margin = new Thickness(0, 12, 0, 0) };
        rule.ColumnDefinitions.Add(new ColumnDefinition());
        rule.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        rule.ColumnDefinitions.Add(new ColumnDefinition());
        var fadeIn = new LinearGradientBrush(Fx.Rgb(Fx.Gold500, 0), Fx.Rgb(Fx.Gold500, 0.55), 0);
        var fadeOut = new LinearGradientBrush(Fx.Rgb(Fx.Gold500, 0.55), Fx.Rgb(Fx.Gold500, 0), 0);
        var l = new Rectangle { Height = 1, Fill = fadeIn, VerticalAlignment = VerticalAlignment.Center };
        var d = new Rectangle { Width = 5, Height = 5, Fill = Fx.Paint(Fx.Gold500), Margin = new Thickness(9, 0, 9, 0), RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new RotateTransform(45), VerticalAlignment = VerticalAlignment.Center };
        var rr = new Rectangle { Height = 1, Fill = fadeOut, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(d, 1);
        Grid.SetColumn(rr, 2);
        rule.Children.Add(l); rule.Children.Add(d); rule.Children.Add(rr);
        name.Children.Add(rule);
        relic.Children.Add(Place(name, WindowWidth - Column, CrestTop + CrestHeight + 30));

        relic.Children.Add(Place(new Motes { Width = Column + 40, Height = WindowHeight - 110 }, WindowWidth - Column - 40, 70));

        Fx.Breathe(halo, UIElement.OpacityProperty, 0.80, 1.0, 6000);
        for (var s = 0; s < 3; s++) Set(s, true, quiet: true);
    }

    /// A stone lit or put out. Lighting it blooms once (unless quiet: the page simply shows it lit)
    public void Set(int s, bool on, bool quiet = false)
    {
        var was = lit[s];
        lit[s] = on;
        Fx.To(stones[s], UIElement.OpacityProperty, on ? 1 : 0, quiet ? 0 : on ? 600 : Fx.Slow);
        if (on) Fx.Breathe(rest[s], UIElement.OpacityProperty, 0.6, 1.0, 4500, s * 1500);
        else Fx.To(rest[s], UIElement.OpacityProperty, 0, quiet ? 0 : Fx.Slow);
        if (on && !was && !quiet && !Fx.Still)
        {
            var grow = (ScaleTransform)bloom[s].RenderTransform;
            Fx.Once(grow, ScaleTransform.ScaleXProperty, 0.45, 1.0, 900);
            Fx.Once(grow, ScaleTransform.ScaleYProperty, 0.45, 1.0, 900);
            Fx.Once(bloom[s], UIElement.OpacityProperty, 0.75, 0, 900, 0, Fx.InOut);
        }
    }

    public void All(bool on, bool quiet = false)
    {
        for (var s = 0; s < 3; s++) Set(s, on, quiet);
    }

    public bool Lit(int s) => lit[s];

    /// A stone filling with light while its step runs (0..1): not lit yet, but on its way
    public void Charge(int s, double f)
    {
        if (lit[s]) return;
        f = Math.Max(0, Math.Min(1, f));
        Fx.To(stones[s], UIElement.OpacityProperty, 0.10 + 0.50 * f, 300);
        Fx.To(rest[s], UIElement.OpacityProperty, 0.15 + 0.35 * f, 300);
    }

    /// The band of light across the gold: when a page arrives with everything in order, and when the work is done
    public void Gleam(int delay = 0)
    {
        if (Fx.Still) return;
        Fx.Once(gleamMove, TranslateTransform.XProperty, BandFrom, BandTo, 1100, delay, Fx.InOut);
        var show = new DoubleAnimationUsingKeyFrames { BeginTime = TimeSpan.FromMilliseconds(delay) };
        show.KeyFrames.Add(new DiscreteDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        show.KeyFrames.Add(new DiscreteDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(1100))));
        gleam.BeginAnimation(UIElement.OpacityProperty, show);
    }

    /// (for still pictures: the band held at a place on its way, 0 = about to enter, 1 = just gone; below 0 puts it away)
    public void GleamAt(double f)
    {
        gleam.BeginAnimation(UIElement.OpacityProperty, null);
        gleamMove.BeginAnimation(TranslateTransform.XProperty, null);
        gleam.Opacity = f < 0 ? 0 : 1;
        gleamMove.X = BandFrom + (BandTo - BandFrom) * Math.Max(0, f);
    }

    /// (for still pictures: the ring of marks turned to an angle, to see that the arc's fade stays where it is)
    public void TurnMarks(double angle)
    {
        marksTurn.BeginAnimation(RotateTransform.AngleProperty, null);
        marksTurn.Angle = angle;
    }

    /// The relic steps back when something needs the whole width (the build's details)
    public void Away(bool away) => Fx.To(relic, UIElement.OpacityProperty, away ? 0 : 1, Fx.Base);
}
