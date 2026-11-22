using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace WinAudio.App.Controls;

// Modern arc-style Rotary Knob for Bass, Treble, Clarity, and Controlled Sound Booster
public sealed class RotaryKnob : FrameworkElement
{
    public static readonly DependencyProperty ValueProperty =
        DependencyProperty.Register(nameof(Value), typeof(double), typeof(RotaryKnob),
            new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault | FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty MinimumProperty =
        DependencyProperty.Register(nameof(Minimum), typeof(double), typeof(RotaryKnob),
            new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty MaximumProperty =
        DependencyProperty.Register(nameof(Maximum), typeof(double), typeof(RotaryKnob),
            new FrameworkPropertyMetadata(15.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty DefaultValueProperty =
        DependencyProperty.Register(nameof(DefaultValue), typeof(double), typeof(RotaryKnob), new PropertyMetadata(0.0));

    public static readonly DependencyProperty LabelProperty =
        DependencyProperty.Register(nameof(Label), typeof(string), typeof(RotaryKnob),
            new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty UnitSuffixProperty =
        DependencyProperty.Register(nameof(UnitSuffix), typeof(string), typeof(RotaryKnob),
            new FrameworkPropertyMetadata(" dB", FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty AccentColorProperty =
        DependencyProperty.Register(nameof(AccentColor), typeof(Color), typeof(RotaryKnob),
            new FrameworkPropertyMetadata(Color.FromRgb(56, 189, 248), FrameworkPropertyMetadataOptions.AffectsRender));

    public double Value { get => (double)GetValue(ValueProperty); set => SetValue(ValueProperty, value); }
    public double Minimum { get => (double)GetValue(MinimumProperty); set => SetValue(MinimumProperty, value); }
    public double Maximum { get => (double)GetValue(MaximumProperty); set => SetValue(MaximumProperty, value); }
    public double DefaultValue { get => (double)GetValue(DefaultValueProperty); set => SetValue(DefaultValueProperty, value); }
    public string Label { get => (string)GetValue(LabelProperty); set => SetValue(LabelProperty, value); }
    public string UnitSuffix { get => (string)GetValue(UnitSuffixProperty); set => SetValue(UnitSuffixProperty, value); }
    public Color AccentColor { get => (Color)GetValue(AccentColorProperty); set => SetValue(AccentColorProperty, value); }

    private bool _dragging;
    private double _dragStartY;
    private double _dragStartVal;

    public RotaryKnob()
    {
        Cursor = Cursors.SizeNS;
        Width = 96;
        Height = 104;
    }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth;
        double h = ActualHeight;
        double cx = w / 2.0;
        double cy = 44.0;
        double r = 32.0;

        // Track arc (135 deg to 405 deg = 270 deg sweep)
        DrawArc(dc, cx, cy, r, 135, 405, new Pen(new SolidColorBrush(Color.FromRgb(42, 52, 72)), 5) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round });

        double norm = Math.Clamp((Value - Minimum) / Math.Max(1e-6, Maximum - Minimum), 0.0, 1.0);
        double endAngle = 135 + norm * 270.0;
        if (norm > 0.005)
        {
            DrawArc(dc, cx, cy, r, 135, endAngle, new Pen(new SolidColorBrush(AccentColor), 5) { StartLineCap = PenLineCap.Round, EndLineCap = PenLineCap.Round });
        }

        // Inner dial body
        var dialBrush = new RadialGradientBrush(Color.FromRgb(38, 48, 68), Color.FromRgb(20, 26, 38));
        dc.DrawEllipse(dialBrush, new Pen(new SolidColorBrush(Color.FromRgb(56, 70, 96)), 1.2), new Point(cx, cy), r - 8, r - 8);

        // Pointer dot
        double rad = endAngle * Math.PI / 180.0;
        Point dot = new(cx + Math.Cos(rad) * (r - 15), cy + Math.Sin(rad) * (r - 15));
        dc.DrawEllipse(new SolidColorBrush(AccentColor), null, dot, 3.2, 3.2);

        double dpi = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        var tf = new Typeface(new FontFamily("Segoe UI Variable Text, Segoe UI"), FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal);
        string valStr = $"{Value:+0.0;-0.0;0.0}{UnitSuffix}";
        if (Minimum >= 0 && !UnitSuffix.Contains("dB")) valStr = $"{Value:0}{UnitSuffix}";
        var valText = new FormattedText(valStr, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, tf, 11.5, Brushes.White, dpi);
        dc.DrawText(valText, new Point(cx - valText.Width / 2, cy + r - 6));

        var lblTf = new Typeface("Segoe UI Variable Text, Segoe UI");
        var lblText = new FormattedText(Label, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, lblTf, 11, new SolidColorBrush(Color.FromRgb(148, 163, 184)), dpi);
        dc.DrawText(lblText, new Point(cx - lblText.Width / 2, h - 16));
    }

    private static void DrawArc(DrawingContext dc, double cx, double cy, double radius, double startDeg, double endDeg, Pen pen)
    {
        double startRad = startDeg * Math.PI / 180.0;
        double endRad = endDeg * Math.PI / 180.0;
        Point start = new(cx + Math.Cos(startRad) * radius, cy + Math.Sin(startRad) * radius);
        Point end = new(cx + Math.Cos(endRad) * radius, cy + Math.Sin(endRad) * radius);
        bool isLarge = (endDeg - startDeg) > 180.0;

        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            ctx.BeginFigure(start, false, false);
            ctx.ArcTo(end, new Size(radius, radius), 0, isLarge, SweepDirection.Clockwise, true, false);
        }
        geo.Freeze();
        dc.DrawGeometry(null, pen, geo);
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonDown(e);
        if (e.ClickCount == 2)
        {
            Value = DefaultValue;
            return;
        }
        _dragging = true;
        _dragStartY = e.GetPosition(this).Y;
        _dragStartVal = Value;
        CaptureMouse();
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (!_dragging) return;
        double dy = _dragStartY - e.GetPosition(this).Y;
        double range = Maximum - Minimum;
        Value = Math.Round(Math.Clamp(_dragStartVal + (dy / 130.0) * range, Minimum, Maximum), 1);
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonUp(e);
        if (_dragging)
        {
            _dragging = false;
            ReleaseMouseCapture();
        }
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        base.OnMouseWheel(e);
        double step = (Maximum - Minimum) / 30.0;
        Value = Math.Round(Math.Clamp(Value + (e.Delta > 0 ? step : -step), Minimum, Maximum), 1);
        e.Handled = true;
    }
}
