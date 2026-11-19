using System;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace A733Installer
{
    /// <summary>Small, quick UI animations. Off when Windows animations are disabled
    /// (Accessibility settings) and for the documentation screenshots.</summary>
    static class Anim
    {
        public static bool Disabled;
        static bool On => !Disabled && SystemParameters.ClientAreaAnimation;
        static readonly IEasingFunction Out = new CubicEase { EasingMode = EasingMode.EaseOut };

        static DoubleAnimation A(double? from, double to, int ms, IEasingFunction ease = null) =>
            new DoubleAnimation { From = from, To = to, Duration = TimeSpan.FromMilliseconds(ms), EasingFunction = ease ?? Out };

        /// <summary>Show an element: fade in while sliding up a few pixels.</summary>
        public static void FadeIn(FrameworkElement el, double dy = 8, int ms = 230)
        {
            el.Visibility = Visibility.Visible;
            if (!On) { el.BeginAnimation(UIElement.OpacityProperty, null); el.Opacity = 1; return; }
            var tt = el.RenderTransform as TranslateTransform;
            if (tt == null) { tt = new TranslateTransform(); el.RenderTransform = tt; }
            el.BeginAnimation(UIElement.OpacityProperty, A(0, 1, ms));
            tt.BeginAnimation(TranslateTransform.YProperty, A(dy, 0, ms));
        }

        /// <summary>A short "spring" when a step badge turns into a check mark.</summary>
        public static void Pop(FrameworkElement el)
        {
            if (!On) return;
            el.RenderTransformOrigin = new Point(0.5, 0.5);
            var st = el.RenderTransform as ScaleTransform;
            if (st == null) { st = new ScaleTransform(1, 1); el.RenderTransform = st; }
            var ease = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.6 };
            st.BeginAnimation(ScaleTransform.ScaleXProperty, A(0.4, 1, 320, ease));
            st.BeginAnimation(ScaleTransform.ScaleYProperty, A(0.4, 1, 320, ease));
        }

        /// <summary>Slow opacity pulse (the step that is running now).</summary>
        public static void Pulse(UIElement el, bool on)
        {
            if (on && On)
            {
                var a = A(1, 0.3, 750, new SineEase { EasingMode = EasingMode.EaseInOut });
                a.AutoReverse = true;
                a.RepeatBehavior = RepeatBehavior.Forever;
                el.BeginAnimation(UIElement.OpacityProperty, a);
            }
            else
            {
                el.BeginAnimation(UIElement.OpacityProperty, null);
                el.Opacity = 1;
            }
        }

        /// <summary>Slide a translate transform horizontally.</summary>
        public static void SlideX(TranslateTransform t, double x)
        {
            if (!On) { t.BeginAnimation(TranslateTransform.XProperty, null); t.X = x; return; }
            t.BeginAnimation(TranslateTransform.XProperty, A(null, x, 260, new CubicEase { EasingMode = EasingMode.EaseInOut }));
        }

        /// <summary>Fade the elements out, run <paramref name="swap"/> (e.g. change texts), fade back in.</summary>
        public static void CrossFade(Action swap, params UIElement[] els)
        {
            if (!On) { swap(); return; }
            var outA = A(null, 0.15, 110);
            outA.Completed += (s, e) =>
            {
                swap();
                foreach (var el in els) el.BeginAnimation(UIElement.OpacityProperty, A(0.15, 1, 180));
            };
            for (int i = 0; i < els.Length; i++)
                els[i].BeginAnimation(UIElement.OpacityProperty, i == 0 ? outA : A(null, 0.15, 110));
        }

        /// <summary>Move a progress bar smoothly to a value.</summary>
        public static void To(RangeBase bar, double value)
        {
            if (!On) { bar.BeginAnimation(RangeBase.ValueProperty, null); bar.Value = value; return; }
            bar.BeginAnimation(RangeBase.ValueProperty, A(null, value, 350));
        }
    }
}
