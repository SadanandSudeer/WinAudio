using System.Windows;
using System.Windows.Media;

namespace WinAudio.App.Controls;

// Smooth horizontal peak/RMS meter or Limiter Gain-Reduction meter
public sealed class LevelMeterControl : FrameworkElement
{
    private static readonly SolidColorBrush BgBrush = Freeze(new SolidColorBrush(Color.FromRgb(15, 23, 42)));
    private static readonly Pen BorderPen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromRgb(42, 52, 72))), 1));
    private static readonly LinearGradientBrush GrBrush = Freeze(new LinearGradientBrush(Color.FromRgb(245, 158, 11), Color.FromRgb(244, 63, 94), 0));
    private static readonly LinearGradientBrush LevelBrush = CreateLevelBrush();

    private static T Freeze<T>(T obj) where T : Freezable { obj.Freeze(); return obj; }
    private static LinearGradientBrush CreateLevelBrush()
    {
        var b = new LinearGradientBrush { StartPoint = new Point(0, 0.5), EndPoint = new Point(1, 0.5) };
        b.GradientStops.Add(new GradientStop(Color.FromRgb(16, 185, 129), 0.0));
        b.GradientStops.Add(new GradientStop(Color.FromRgb(56, 189, 248), 0.72));
        b.GradientStops.Add(new GradientStop(Color.FromRgb(245, 158, 11), 0.90));
        b.GradientStops.Add(new GradientStop(Color.FromRgb(244, 63, 94), 1.0));
        b.Freeze();
        return b;
    }

    public static readonly DependencyProperty LevelDbProperty =
        DependencyProperty.Register(nameof(LevelDb), typeof(double), typeof(LevelMeterControl),
            new FrameworkPropertyMetadata(-90.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty IsGainReductionProperty =
        DependencyProperty.Register(nameof(IsGainReduction), typeof(bool), typeof(LevelMeterControl),
            new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender));

    public double LevelDb
    {
        get => (double)GetValue(LevelDbProperty);
        set => SetValue(LevelDbProperty, value);
    }

    public bool IsGainReduction
    {
        get => (bool)GetValue(IsGainReductionProperty);
        set => SetValue(IsGainReductionProperty, value);
    }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth;
        double h = ActualHeight;
        if (w <= 2 || h <= 2) return;

        dc.DrawRoundedRectangle(BgBrush, BorderPen, new Rect(0, 0, w, h), 4, 4);

        if (IsGainReduction)
        {
            double norm = Math.Clamp(-LevelDb / 18.0, 0.0, 1.0);
            double fillW = Math.Max(0, (w - 2) * norm);
            if (fillW > 0.5)
            {
                dc.DrawRoundedRectangle(GrBrush, null, new Rect(w - 1 - fillW, 1, fillW, h - 2), 3, 3);
            }
        }
        else
        {
            double norm = Math.Clamp((LevelDb + 60.0) / 60.0, 0.0, 1.0);
            double fillW = Math.Max(0, (w - 2) * norm);
            if (fillW > 0.5)
            {
                dc.DrawRoundedRectangle(LevelBrush, null, new Rect(1, 1, fillW, h - 2), 3, 3);
            }
        }
    }
}
