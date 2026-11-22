using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using WinAudio.App.Interop;
using WinAudio.App.Models;

namespace WinAudio.App.Controls;

// High-DPI Interactive Logarithmic EQ Curve + Live 64-Band FFT Spectrum Canvas (20 Hz .. 20 kHz, -18 .. +18 dB)
// Uses frozen Brushes/Pens and cached FormattedText axis labels for ultra-low (< 1%) UI CPU usage.
public sealed class EqCurveCanvas : FrameworkElement
{
    public static readonly double[] Freqs10 = { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    public static readonly double[] Freqs31 = {
        20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
        200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
        2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
    };

    private const double MinFreq = 20.0;
    private const double MaxFreq = 20000.0;
    private const double MinDb = -18.0;
    private const double MaxDb = 18.0;

    private static readonly double[] DbMarks = { 18, 12, 6, 0, -6, -12, -18 };
    private static readonly (double freq, string label)[] FreqMarks =
    {
        (20, "20"), (50, "50"), (100, "100"), (200, "200"), (500, "500"),
        (1000, "1k"), (2000, "2k"), (5000, "5k"), (10000, "10k"), (20000, "20k")
    };

    // Frozen static drawing resources (zero per-frame allocations!)
    private static readonly LinearGradientBrush BgBrush = Freeze(new LinearGradientBrush(Color.FromRgb(12, 16, 24), Color.FromRgb(18, 24, 36), 90));
    private static readonly Pen BorderPen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromRgb(42, 52, 72))), 1));
    private static readonly Pen GridPen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromArgb(42, 148, 163, 184))), 1));
    private static readonly Pen ZeroPen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromArgb(110, 148, 163, 184))), 1.2));
    private static readonly SolidColorBrush LabelBrush = Freeze(new SolidColorBrush(Color.FromRgb(100, 116, 139)));
    private static readonly LinearGradientBrush SpecFillBrush = Freeze(new LinearGradientBrush(Color.FromArgb(90, 56, 189, 248), Color.FromArgb(14, 16, 185, 129), 90));
    private static readonly Pen SpecStrokePen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromArgb(110, 56, 189, 248))), 1));
    private static readonly SolidColorBrush CurveFillBrush = Freeze(new SolidColorBrush(Color.FromArgb(40, 16, 185, 129)));
    private static readonly Pen CurveGlowPen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromArgb(75, 16, 185, 129))), 4.5));
    private static readonly Pen CurveLinePen = Freeze(new Pen(Freeze(new SolidColorBrush(Color.FromRgb(52, 211, 153))), 2.1));
    private static readonly SolidColorBrush NodeIdleBrush = Freeze(new SolidColorBrush(Color.FromRgb(16, 185, 129)));
    private static readonly SolidColorBrush NodeHotBrush = Freeze(new SolidColorBrush(Color.FromRgb(56, 189, 248)));
    private static readonly SolidColorBrush NodeMutedBrush = Freeze(new SolidColorBrush(Color.FromRgb(71, 85, 105)));
    private static readonly SolidColorBrush TooltipBgBrush = Freeze(new SolidColorBrush(Color.FromArgb(225, 15, 23, 42)));
    private static readonly Pen NodeRingPen = Freeze(new Pen(Brushes.White, 1.2));
    private static readonly Pen NodeHotRingPen = Freeze(new Pen(Brushes.White, 2.0));
    private static readonly Typeface AxisTypeface = new("Segoe UI");

    private static T Freeze<T>(T obj) where T : Freezable { obj.Freeze(); return obj; }

    private FormattedText[]? _cachedDbLabels;
    private FormattedText[]? _cachedFreqLabels;
    private double _cachedDpi;

    private readonly float[] _spectrumDb = new float[EngineNative.SpectrumBins];
    private float[] _curveFreqs = Array.Empty<float>();
    private float[] _curveDb = Array.Empty<float>();
    private AudioProfile? _profile;
    private int _dragIndex = -1;
    private int _hoverIndex = -1;

    public event Action<int, float, float, float>? BandChanged;
    public event Action<int>? BandSelected;

    public EqCurveCanvas()
    {
        ClipToBounds = true;
        Cursor = Cursors.Cross;
        Array.Fill(_spectrumDb, -90f);
        BuildCurveGridFreqs();
    }

    private void BuildCurveGridFreqs()
    {
        const int n = 128;
        _curveFreqs = new float[n];
        _curveDb = new float[n];
        double logRatio = Math.Log(MaxFreq / MinFreq);
        for (int i = 0; i < n; i++)
        {
            _curveFreqs[i] = (float)(MinFreq * Math.Exp(logRatio * i / (n - 1)));
        }
    }

    public float[] CurveFrequencies => _curveFreqs;

    public void UpdateVisuals(AudioProfile profile, float[]? curveDb, float[]? spectrumDb)
    {
        _profile = profile;
        if (curveDb != null && curveDb.Length == _curveDb.Length)
            Array.Copy(curveDb, _curveDb, curveDb.Length);
        if (spectrumDb != null && spectrumDb.Length == _spectrumDb.Length)
            Array.Copy(spectrumDb, _spectrumDb, spectrumDb.Length);
        if (IsVisible) InvalidateVisual();
    }

    public void UpdateSpectrumOnly(float[] spectrumDb)
    {
        if (!IsVisible || spectrumDb.Length != _spectrumDb.Length) return;

        bool changed = false;
        for (int i = 0; i < _spectrumDb.Length; i++)
        {
            if (Math.Abs(_spectrumDb[i] - spectrumDb[i]) > 0.35f)
            {
                changed = true;
                break;
            }
        }
        if (!changed) return;

        Array.Copy(spectrumDb, _spectrumDb, spectrumDb.Length);
        InvalidateVisual();
    }

    private void EnsureCachedLabels(double dpi)
    {
        if (_cachedDbLabels != null && Math.Abs(_cachedDpi - dpi) < 0.01) return;
        _cachedDpi = dpi;
        _cachedDbLabels = DbMarks.Select(db =>
            new FormattedText($"{db:+0;-0;0}", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, AxisTypeface, 10, LabelBrush, dpi)).ToArray();
        _cachedFreqLabels = FreqMarks.Select(m =>
            new FormattedText(m.label, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, AxisTypeface, 10, LabelBrush, dpi)).ToArray();
    }

    private double FreqToX(double freq, double w)
    {
        double t = Math.Log(Math.Clamp(freq, MinFreq, MaxFreq) / MinFreq) / Math.Log(MaxFreq / MinFreq);
        return 36 + t * Math.Max(10, w - 52);
    }

    private double XToFreq(double x, double w)
    {
        double t = Math.Clamp((x - 36) / Math.Max(10, w - 52), 0.0, 1.0);
        return MinFreq * Math.Pow(MaxFreq / MinFreq, t);
    }

    private double DbToY(double db, double h)
    {
        double t = (MaxDb - Math.Clamp(db, MinDb, MaxDb)) / (MaxDb - MinDb);
        return 14 + t * Math.Max(10, h - 36);
    }

    private double YToDb(double y, double h)
    {
        double t = Math.Clamp((y - 14) / Math.Max(10, h - 36), 0.0, 1.0);
        return MaxDb - t * (MaxDb - MinDb);
    }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth;
        double h = ActualHeight;
        if (w < 40 || h < 40) return;

        double dpi = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        EnsureCachedLabels(dpi);

        dc.DrawRoundedRectangle(BgBrush, BorderPen, new Rect(0, 0, w, h), 8, 8);

        for (int i = 0; i < DbMarks.Length; i++)
        {
            double y = DbToY(DbMarks[i], h);
            dc.DrawLine(DbMarks[i] == 0 ? ZeroPen : GridPen, new Point(36, y), new Point(w - 16, y));
            dc.DrawText(_cachedDbLabels![i], new Point(6, y - 6));
        }

        for (int i = 0; i < FreqMarks.Length; i++)
        {
            double x = FreqToX(FreqMarks[i].freq, w);
            dc.DrawLine(GridPen, new Point(x, 14), new Point(x, h - 22));
            var txt = _cachedFreqLabels![i];
            dc.DrawText(txt, new Point(x - txt.Width / 2, h - 18));
        }

        // 1. Live 64-Band FFT Spectrum Backdrop
        bool anyEnergy = false;
        for (int i = 0; i < _spectrumDb.Length; i++) { if (_spectrumDb[i] > -84f) { anyEnergy = true; break; } }

        if (anyEnergy)
        {
            double plotBottom = h - 22;
            double plotHeight = Math.Max(10, plotBottom - 14);
            var specGeo = new StreamGeometry();
            using (var ctx = specGeo.Open())
            {
                ctx.BeginFigure(new Point(FreqToX(MinFreq, w), plotBottom), true, true);
                for (int i = 0; i < _spectrumDb.Length; i++)
                {
                    double t = (double)i / (_spectrumDb.Length - 1);
                    double freq = MinFreq * Math.Pow(MaxFreq / MinFreq, t);
                    double x = FreqToX(freq, w);
                    double norm = Math.Clamp((_spectrumDb[i] + 85.0) / 85.0, 0.0, 1.0);
                    double y = plotBottom - norm * plotHeight;
                    ctx.LineTo(new Point(x, y), true, false);
                }
                ctx.LineTo(new Point(FreqToX(MaxFreq, w), plotBottom), true, false);
            }
            specGeo.Freeze();
            dc.DrawGeometry(SpecFillBrush, SpecStrokePen, specGeo);
        }

        // 2. Analytical EQ Response Curve
        if (_curveFreqs.Length > 1)
        {
            var curveGeo = new StreamGeometry();
            var fillGeo = new StreamGeometry();
            double zeroY = DbToY(0, h);

            using (var ctx = curveGeo.Open())
            using (var fctx = fillGeo.Open())
            {
                Point p0 = new(FreqToX(_curveFreqs[0], w), DbToY(_curveDb[0], h));
                ctx.BeginFigure(p0, false, false);
                fctx.BeginFigure(new Point(p0.X, zeroY), true, true);
                fctx.LineTo(p0, true, false);

                for (int i = 1; i < _curveFreqs.Length; i++)
                {
                    Point pt = new(FreqToX(_curveFreqs[i], w), DbToY(_curveDb[i], h));
                    ctx.LineTo(pt, true, true);
                    fctx.LineTo(pt, true, true);
                }
                fctx.LineTo(new Point(FreqToX(_curveFreqs[^1], w), zeroY), true, false);
            }
            curveGeo.Freeze();
            fillGeo.Freeze();

            dc.DrawGeometry(CurveFillBrush, null, fillGeo);
            dc.DrawGeometry(null, CurveGlowPen, curveGeo);
            dc.DrawGeometry(null, CurveLinePen, curveGeo);
        }

        // 3. Interactive Draggable Band Nodes
        if (_profile?.Eq.Enabled == true)
        {
            var nodes = GetNodes(w, h);
            for (int i = 0; i < nodes.Count; i++)
            {
                var (pt, active, idx) = nodes[i];
                bool isHot = (idx == _dragIndex || idx == _hoverIndex);
                double r = _profile.Eq.Mode == WaEqMode.Eq31Band ? (isHot ? 5.5 : 3.8) : (isHot ? 7.5 : 5.5);

                Brush fill = !active ? NodeMutedBrush : (isHot ? NodeHotBrush : NodeIdleBrush);
                dc.DrawEllipse(fill, isHot ? NodeHotRingPen : NodeRingPen, pt, r, r);

                if (isHot && _profile.Eq.Mode != WaEqMode.Eq31Band)
                {
                    var label = GetNodeLabel(idx);
                    var ft = new FormattedText(label, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, AxisTypeface, 10.5, Brushes.White, dpi);
                    double bx = Math.Clamp(pt.X - ft.Width / 2 - 5, 38, w - ft.Width - 26);
                    double by = Math.Max(16, pt.Y - 26);
                    dc.DrawRoundedRectangle(TooltipBgBrush, BorderPen, new Rect(bx, by, ft.Width + 10, 18), 4, 4);
                    dc.DrawText(ft, new Point(bx + 5, by + 2));
                }
            }
        }
    }

    private string GetNodeLabel(int idx)
    {
        if (_profile == null) return "";
        if (_profile.Eq.Mode == WaEqMode.Eq10Band && idx < Freqs10.Length)
            return $"{FormatFreq(Freqs10[idx])} • {_profile.Eq.Graphic10Db[idx]:+0.0;-0.0;0.0} dB";
        if (_profile.Eq.Mode == WaEqMode.Parametric && idx < _profile.Eq.ParametricBands.Count)
        {
            var b = _profile.Eq.ParametricBands[idx];
            return $"B{idx + 1} {FormatFreq(b.FrequencyHz)} • {b.GainDb:+0.0;-0.0;0.0} dB • Q {b.Q:0.0}";
        }
        return "";
    }

    private static string FormatFreq(double f) => f >= 1000 ? $"{f / 1000.0:0.##}kHz" : $"{f:0}Hz";

    private List<(Point pt, bool active, int idx)> GetNodes(double w, double h)
    {
        var list = new List<(Point, bool, int)>();
        if (_profile == null) return list;

        if (_profile.Eq.Mode == WaEqMode.Eq10Band)
        {
            for (int i = 0; i < Freqs10.Length; i++)
                list.Add((new Point(FreqToX(Freqs10[i], w), DbToY(_profile.Eq.Graphic10Db[i], h)), true, i));
        }
        else if (_profile.Eq.Mode == WaEqMode.Eq31Band)
        {
            for (int i = 0; i < Freqs31.Length; i++)
                list.Add((new Point(FreqToX(Freqs31[i], w), DbToY(_profile.Eq.Graphic31Db[i], h)), true, i));
        }
        else
        {
            for (int i = 0; i < _profile.Eq.ParametricBands.Count; i++)
            {
                var b = _profile.Eq.ParametricBands[i];
                list.Add((new Point(FreqToX(b.FrequencyHz, w), DbToY(b.GainDb, h)), b.Enabled, i));
            }
        }
        return list;
    }

    private int HitTestNode(Point pos)
    {
        var nodes = GetNodes(ActualWidth, ActualHeight);
        int best = -1;
        double bestDist = 18.0;
        foreach (var (pt, _, idx) in nodes)
        {
            double d = (pt - pos).Length;
            if (d < bestDist)
            {
                bestDist = d;
                best = idx;
            }
        }
        return best;
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonDown(e);
        int hit = HitTestNode(e.GetPosition(this));
        if (hit >= 0)
        {
            BandSelected?.Invoke(hit);
            if (e.ClickCount == 2)
            {
                EmitBandChange(hit, null, 0f, null);
                return;
            }
            _dragIndex = hit;
            CaptureMouse();
            InvalidateVisual();
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        Point pos = e.GetPosition(this);
        if (_dragIndex >= 0 && _profile != null)
        {
            float gain = (float)Math.Clamp(YToDb(pos.Y, ActualHeight), -15.0, 15.0);
            float? freq = _profile.Eq.Mode == WaEqMode.Parametric
                ? (float)Math.Clamp(XToFreq(pos.X, ActualWidth), 20.0, 20000.0)
                : null;
            EmitBandChange(_dragIndex, freq, gain, null);
        }
        else
        {
            int hover = HitTestNode(pos);
            if (hover != _hoverIndex)
            {
                _hoverIndex = hover;
                InvalidateVisual();
            }
        }
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonUp(e);
        if (_dragIndex >= 0)
        {
            _dragIndex = -1;
            ReleaseMouseCapture();
            InvalidateVisual();
        }
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        base.OnMouseWheel(e);
        if (_profile?.Eq.Mode != WaEqMode.Parametric) return;
        int target = _dragIndex >= 0 ? _dragIndex : HitTestNode(e.GetPosition(this));
        if (target >= 0 && target < _profile.Eq.ParametricBands.Count)
        {
            var b = _profile.Eq.ParametricBands[target];
            float newQ = Math.Clamp(b.Q + (e.Delta > 0 ? 0.25f : -0.25f), 0.3f, 12.0f);
            EmitBandChange(target, null, null, newQ);
            e.Handled = true;
        }
    }

    private void EmitBandChange(int idx, float? freq, float? gain, float? q)
    {
        if (_profile == null) return;
        if (_profile.Eq.Mode == WaEqMode.Eq10Band && idx < 10)
        {
            float g = Math.Clamp(gain ?? _profile.Eq.Graphic10Db[idx], -15f, 15f);
            BandChanged?.Invoke(idx, (float)Freqs10[idx], g, 1.414f);
        }
        else if (_profile.Eq.Mode == WaEqMode.Eq31Band && idx < 31)
        {
            float g = Math.Clamp(gain ?? _profile.Eq.Graphic31Db[idx], -15f, 15f);
            BandChanged?.Invoke(idx, (float)Freqs31[idx], g, 4.318f);
        }
        else if (_profile.Eq.Mode == WaEqMode.Parametric && idx < _profile.Eq.ParametricBands.Count)
        {
            var b = _profile.Eq.ParametricBands[idx];
            BandChanged?.Invoke(idx, freq ?? b.FrequencyHz, Math.Clamp(gain ?? b.GainDb, -18f, 18f), q ?? b.Q);
        }
    }
}
