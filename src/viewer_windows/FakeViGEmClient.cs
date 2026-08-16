using System;

namespace Nefarius.ViGEm.Client
{
    public sealed class ViGEmClient : IDisposable
    {
        public Targets.Xbox360.Xbox360Controller CreateXbox360Controller()
        {
            return new Targets.Xbox360.Xbox360Controller();
        }

        public void Dispose() { }
    }
}

namespace Nefarius.ViGEm.Client.Targets.Xbox360
{
    public sealed class Xbox360Button
    {
        public static readonly Xbox360Button Up = new Xbox360Button();
        public static readonly Xbox360Button Down = new Xbox360Button();
        public static readonly Xbox360Button Left = new Xbox360Button();
        public static readonly Xbox360Button Right = new Xbox360Button();
        public static readonly Xbox360Button Start = new Xbox360Button();
        public static readonly Xbox360Button Back = new Xbox360Button();
        public static readonly Xbox360Button LeftThumb = new Xbox360Button();
        public static readonly Xbox360Button RightThumb = new Xbox360Button();
        public static readonly Xbox360Button LeftShoulder = new Xbox360Button();
        public static readonly Xbox360Button RightShoulder = new Xbox360Button();
        public static readonly Xbox360Button Guide = new Xbox360Button();
        public static readonly Xbox360Button A = new Xbox360Button();
        public static readonly Xbox360Button B = new Xbox360Button();
        public static readonly Xbox360Button X = new Xbox360Button();
        public static readonly Xbox360Button Y = new Xbox360Button();
    }

    public sealed class Xbox360Slider
    {
        public static readonly Xbox360Slider LeftTrigger = new Xbox360Slider();
        public static readonly Xbox360Slider RightTrigger = new Xbox360Slider();
    }

    public sealed class Xbox360Axis
    {
        public static readonly Xbox360Axis LeftThumbX = new Xbox360Axis();
        public static readonly Xbox360Axis LeftThumbY = new Xbox360Axis();
        public static readonly Xbox360Axis RightThumbX = new Xbox360Axis();
        public static readonly Xbox360Axis RightThumbY = new Xbox360Axis();
    }

    public sealed class Xbox360Controller : IDisposable
    {
        public bool AutoSubmitReport { get; set; }
        public void Connect() { }
        public void ResetReport() { }
        public void SubmitReport() { }
        public void SetButtonState(Xbox360Button button, bool pressed) { }
        public void SetSliderValue(Xbox360Slider slider, byte value) { }
        public void SetAxisValue(Xbox360Axis axis, short value) { }
        public void Dispose() { }
    }
}
