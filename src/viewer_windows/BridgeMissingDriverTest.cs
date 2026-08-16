using PS3xPADViewer;
using System;

internal static class BridgeMissingDriverTest
{
    public static int Main(string[] args)
    {
        if (args.Length != 1) return 2;
        using (VirtualGamepadBridge bridge = new VirtualGamepadBridge())
        {
            bridge.Start(args[0]);
            Console.WriteLine(bridge.StatusText);
            return bridge.StatusText.IndexOf("driver ViGEmBus ausente", StringComparison.OrdinalIgnoreCase) >= 0 ? 0 : 1;
        }
    }
}
