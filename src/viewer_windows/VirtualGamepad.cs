using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;

namespace PS3xPADViewer
{
    internal sealed class X360State
    {
        public ushort Buttons;
        public byte LeftTrigger;
        public byte RightTrigger;
        public short LeftX;
        public short LeftY;
        public short RightX;
        public short RightY;

        public bool SameAs(X360State other)
        {
            return other != null && Buttons == other.Buttons &&
                LeftTrigger == other.LeftTrigger && RightTrigger == other.RightTrigger &&
                LeftX == other.LeftX && LeftY == other.LeftY &&
                RightX == other.RightX && RightY == other.RightY;
        }
    }

    internal static class X360Translator
    {
        internal const ushort DpadUp = 0x0001;
        internal const ushort DpadDown = 0x0002;
        internal const ushort DpadLeft = 0x0004;
        internal const ushort DpadRight = 0x0008;
        internal const ushort Start = 0x0010;
        internal const ushort Back = 0x0020;
        internal const ushort LeftThumb = 0x0040;
        internal const ushort RightThumb = 0x0080;
        internal const ushort LeftShoulder = 0x0100;
        internal const ushort RightShoulder = 0x0200;
        internal const ushort Guide = 0x0400;
        internal const ushort A = 0x1000;
        internal const ushort B = 0x2000;
        internal const ushort X = 0x4000;
        internal const ushort Y = 0x8000;

        internal static X360State Translate(PadState pad)
        {
            X360State result = new X360State();
            if (pad == null || !pad.Connected) return result;

            int source = pad.Buttons;
            if ((source & 0x0001) != 0) result.Buttons |= A;
            if ((source & 0x0002) != 0) result.Buttons |= B;
            if ((source & 0x0004) != 0) result.Buttons |= X;
            if ((source & 0x0008) != 0) result.Buttons |= Y;
            if ((source & 0x0010) != 0) result.Buttons |= LeftShoulder;
            if ((source & 0x0020) != 0) result.Buttons |= RightShoulder;
            if ((source & 0x0040) != 0) result.Buttons |= Back;
            if ((source & 0x0080) != 0) result.Buttons |= Start;
            if ((source & 0x0100) != 0) result.Buttons |= LeftThumb;
            if ((source & 0x0200) != 0) result.Buttons |= RightThumb;
            if ((source & 0x0400) != 0) result.Buttons |= DpadUp;
            if ((source & 0x0800) != 0) result.Buttons |= DpadDown;
            if ((source & 0x1000) != 0) result.Buttons |= DpadLeft;
            if ((source & 0x2000) != 0) result.Buttons |= DpadRight;
            if ((source & 0x4000) != 0) result.Buttons |= Guide;

            result.LeftTrigger = ClampByte(pad.L2);
            result.RightTrigger = ClampByte(pad.R2);
            result.LeftX = Axis(pad.LX, false);
            result.LeftY = Axis(pad.LY, true);
            result.RightX = Axis(pad.RX, false);
            result.RightY = Axis(pad.RY, true);
            return result;
        }

        internal static short Axis(int value, bool invert)
        {
            int clamped = Math.Max(0, Math.Min(255, value));
            int converted;
            if (clamped >= 128)
                converted = (clamped - 128) * 32767 / 127;
            else
                converted = (clamped - 128) * 32768 / 128;
            if (invert)
            {
                converted = -converted;
                if (converted > 32767) converted = 32767;
                if (converted < -32768) converted = -32768;
            }
            return (short)converted;
        }

        private static byte ClampByte(int value)
        {
            return (byte)Math.Max(0, Math.Min(255, value));
        }
    }

    internal sealed class VirtualGamepadBridge : IDisposable
    {
        private readonly object sync = new object();
        private object client;
        private object controller;
        private MethodInfo resetReport;
        private MethodInfo submitReport;
        private MethodInfo setButton;
        private MethodInfo setSlider;
        private MethodInfo setAxis;
        private Type buttonType;
        private Type sliderType;
        private Type axisType;
        private Dictionary<string, object> buttonValues;
        private Dictionary<string, object> sliderValues;
        private Dictionary<string, object> axisValues;
        private X360State lastState;
        private string status = "Controle virtual: inicializando...";

        public string StatusText
        {
            get { lock (sync) return status; }
        }

        public void Start(string root)
        {
            lock (sync)
            {
                CloseObjects();
                string libraryPath = Path.Combine(root, "Nefarius.ViGEm.Client.dll");
                if (!File.Exists(libraryPath))
                {
                    status = "Controle virtual: componente não instalado; execute o instalador novamente";
                    return;
                }
                try
                {
                    Assembly library = Assembly.LoadFrom(libraryPath);
                    Type clientType = RequiredType(library, "Nefarius.ViGEm.Client.ViGEmClient");
                    buttonType = RequiredType(library, "Nefarius.ViGEm.Client.Targets.Xbox360.Xbox360Button");
                    sliderType = RequiredType(library, "Nefarius.ViGEm.Client.Targets.Xbox360.Xbox360Slider");
                    axisType = RequiredType(library, "Nefarius.ViGEm.Client.Targets.Xbox360.Xbox360Axis");

                    client = Activator.CreateInstance(clientType);
                    MethodInfo create = clientType.GetMethod("CreateXbox360Controller", Type.EmptyTypes);
                    if (create == null) throw new MissingMethodException(clientType.FullName, "CreateXbox360Controller");
                    controller = create.Invoke(client, null);
                    Type controllerType = controller.GetType();
                    PropertyInfo autoSubmit = controllerType.GetProperty("AutoSubmitReport");
                    if (autoSubmit != null && autoSubmit.CanWrite) autoSubmit.SetValue(controller, false, null);
                    resetReport = RequiredMethod(controllerType, "ResetReport", Type.EmptyTypes);
                    submitReport = RequiredMethod(controllerType, "SubmitReport", Type.EmptyTypes);
                    setButton = RequiredMethod(controllerType, "SetButtonState", new Type[] { buttonType, typeof(bool) });
                    setSlider = RequiredMethod(controllerType, "SetSliderValue", new Type[] { sliderType, typeof(byte) });
                    setAxis = RequiredMethod(controllerType, "SetAxisValue", new Type[] { axisType, typeof(short) });
                    buttonValues = LoadValues(buttonType, new string[] {
                        "Up", "Down", "Left", "Right", "Start", "Back", "LeftThumb", "RightThumb",
                        "LeftShoulder", "RightShoulder", "Guide", "A", "B", "X", "Y" });
                    sliderValues = LoadValues(sliderType, new string[] { "LeftTrigger", "RightTrigger" });
                    axisValues = LoadValues(axisType, new string[] {
                        "LeftThumbX", "LeftThumbY", "RightThumbX", "RightThumbY" });
                    RequiredMethod(controllerType, "Connect", Type.EmptyTypes).Invoke(controller, null);
                    Send(new X360State());
                    status = "Controle virtual: Xbox 360 ativo (espelhando o Pad 1)";
                }
                catch (Exception ex)
                {
                    string reason = Explain(ex);
                    CloseObjects();
                    status = "Controle virtual: " + reason;
                }
            }
        }

        public void Update(PadState pad)
        {
            if (pad == null || pad.Pad != 0) return;
            lock (sync)
            {
                if (controller == null) return;
                X360State translated = X360Translator.Translate(pad);
                if (translated.SameAs(lastState)) return;
                try
                {
                    Send(translated);
                }
                catch (Exception ex)
                {
                    status = "Controle virtual: comunicação interrompida (" + Explain(ex) + ")";
                    CloseObjects();
                }
            }
        }

        private void Send(X360State state)
        {
            resetReport.Invoke(controller, null);
            SetButton(state.Buttons, X360Translator.DpadUp, "Up");
            SetButton(state.Buttons, X360Translator.DpadDown, "Down");
            SetButton(state.Buttons, X360Translator.DpadLeft, "Left");
            SetButton(state.Buttons, X360Translator.DpadRight, "Right");
            SetButton(state.Buttons, X360Translator.Start, "Start");
            SetButton(state.Buttons, X360Translator.Back, "Back");
            SetButton(state.Buttons, X360Translator.LeftThumb, "LeftThumb");
            SetButton(state.Buttons, X360Translator.RightThumb, "RightThumb");
            SetButton(state.Buttons, X360Translator.LeftShoulder, "LeftShoulder");
            SetButton(state.Buttons, X360Translator.RightShoulder, "RightShoulder");
            SetButton(state.Buttons, X360Translator.Guide, "Guide");
            SetButton(state.Buttons, X360Translator.A, "A");
            SetButton(state.Buttons, X360Translator.B, "B");
            SetButton(state.Buttons, X360Translator.X, "X");
            SetButton(state.Buttons, X360Translator.Y, "Y");
            setSlider.Invoke(controller, new object[] { sliderValues["LeftTrigger"], state.LeftTrigger });
            setSlider.Invoke(controller, new object[] { sliderValues["RightTrigger"], state.RightTrigger });
            setAxis.Invoke(controller, new object[] { axisValues["LeftThumbX"], state.LeftX });
            setAxis.Invoke(controller, new object[] { axisValues["LeftThumbY"], state.LeftY });
            setAxis.Invoke(controller, new object[] { axisValues["RightThumbX"], state.RightX });
            setAxis.Invoke(controller, new object[] { axisValues["RightThumbY"], state.RightY });
            submitReport.Invoke(controller, null);
            lastState = state;
        }

        private void SetButton(ushort buttons, ushort mask, string name)
        {
            if ((buttons & mask) != 0)
                setButton.Invoke(controller, new object[] { buttonValues[name], true });
        }

        private static Dictionary<string, object> LoadValues(Type type, string[] names)
        {
            Dictionary<string, object> values = new Dictionary<string, object>(StringComparer.Ordinal);
            for (int i = 0; i < names.Length; i++)
            {
                FieldInfo field = type.GetField(names[i], BindingFlags.Public | BindingFlags.Static);
                if (field == null) throw new MissingFieldException(type.FullName, names[i]);
                object value = field.GetValue(null);
                if (value == null) throw new InvalidOperationException(type.FullName + "." + names[i] + " retornou null");
                values.Add(names[i], value);
            }
            return values;
        }

        private static Type RequiredType(Assembly assembly, string name)
        {
            Type type = assembly.GetType(name, false);
            if (type == null) throw new TypeLoadException(name);
            return type;
        }

        private static MethodInfo RequiredMethod(Type type, string name, Type[] parameters)
        {
            MethodInfo method = type.GetMethod(name, parameters);
            if (method == null) throw new MissingMethodException(type.FullName, name);
            return method;
        }

        private static string Explain(Exception exception)
        {
            Exception current = exception;
            while (current is TargetInvocationException && current.InnerException != null)
                current = current.InnerException;
            string typeName = current.GetType().Name;
            if (typeName.IndexOf("VigemBusNotFound", StringComparison.OrdinalIgnoreCase) >= 0)
                return "driver ViGEmBus ausente; execute o instalador novamente";
            if (current is BadImageFormatException)
                return "biblioteca incompatível com este Windows";
            if (current is FileLoadException || current is FileNotFoundException)
                return "biblioteca do controle virtual incompleta";
            return current.Message;
        }

        private void CloseObjects()
        {
            lastState = null;
            if (controller != null)
            {
                try
                {
                    IDisposable disposable = controller as IDisposable;
                    if (disposable != null) disposable.Dispose();
                    else
                    {
                        MethodInfo disconnect = controller.GetType().GetMethod("Disconnect", Type.EmptyTypes);
                        if (disconnect != null) disconnect.Invoke(controller, null);
                    }
                }
                catch { }
            }
            if (client != null)
            {
                try
                {
                    IDisposable disposable = client as IDisposable;
                    if (disposable != null) disposable.Dispose();
                }
                catch { }
            }
            controller = null;
            client = null;
            resetReport = submitReport = setButton = setSlider = setAxis = null;
            buttonType = sliderType = axisType = null;
            buttonValues = sliderValues = axisValues = null;
        }

        public void Dispose()
        {
            lock (sync)
            {
                CloseObjects();
                status = "Controle virtual: parado";
            }
        }
    }
}
