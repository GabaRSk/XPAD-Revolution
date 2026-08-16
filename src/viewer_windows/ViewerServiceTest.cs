using PS3xPADViewer;
using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

internal static class ViewerServiceTest
{
    public static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("content root required");
            return 2;
        }
        const int testUdpPort = 39100;
        const int testDiscoveryPort = 39101;
        const int testHttpPort = 8766;
        ViewerService service = new ViewerService(args[0], testUdpPort, testHttpPort);
        try
        {
            service.Start();
            if (!service.StatusText().Contains("Controle virtual: Xbox 360 ativo"))
                throw new Exception("virtual controller reflection bridge did not start: " + service.StatusText());
            IPEndPoint discoverySender;
            using (UdpClient discoveryReceiver = new UdpClient(new IPEndPoint(IPAddress.Loopback, testDiscoveryPort)))
            {
                discoveryReceiver.Client.ReceiveTimeout = 2000;
                service.SendDiscoveryTo(new IPEndPoint(IPAddress.Loopback, testDiscoveryPort));
                discoverySender = new IPEndPoint(IPAddress.Any, 0);
                byte[] discovery = discoveryReceiver.Receive(ref discoverySender);
                if (discovery.Length != 8 || discovery[0] != (byte)'X' || discovery[1] != (byte)'P' ||
                    discovery[2] != (byte)'V' || discovery[3] != (byte)'D' || discovery[4] != 1)
                    throw new Exception("XPVD discovery packet mismatch");
                if (discoverySender.Port != testUdpPort)
                    throw new Exception("discovery source port mismatch: " + discoverySender.Port);
            }
            IPAddress calculated = ViewerService.CalculateBroadcast(
                IPAddress.Parse("192.168.44.23"), IPAddress.Parse("255.255.255.0"));
            if (calculated == null || calculated.ToString() != "192.168.44.255")
                throw new Exception("directed broadcast calculation mismatch");

            PadState translationInput = new PadState();
            translationInput.Connected = true;
            translationInput.Buttons = 0x0001 | 0x0020 | 0x1000 | 0x4000;
            translationInput.L2 = 64;
            translationInput.R2 = 192;
            translationInput.LX = 0;
            translationInput.LY = 0;
            translationInput.RX = 255;
            translationInput.RY = 255;
            X360State translated = X360Translator.Translate(translationInput);
            ushort expectedButtons = X360Translator.A | X360Translator.RightShoulder |
                X360Translator.DpadLeft | X360Translator.Guide;
            if (translated.Buttons != expectedButtons || translated.LeftTrigger != 64 || translated.RightTrigger != 192)
                throw new Exception("X360 button/trigger translation mismatch");
            if (translated.LeftX != -32768 || translated.LeftY != 32767 ||
                translated.RightX != 32767 || translated.RightY != -32767 || X360Translator.Axis(128, false) != 0)
                throw new Exception("X360 axis translation mismatch");
            translationInput.Connected = false;
            X360State neutral = X360Translator.Translate(translationInput);
            if (neutral.Buttons != 0 || neutral.LeftTrigger != 0 || neutral.LeftX != 0)
                throw new Exception("X360 disconnect neutralization mismatch");
            byte[] packet = new byte[18];
            packet[0] = (byte)'X'; packet[1] = (byte)'P'; packet[2] = (byte)'V'; packet[3] = (byte)'3';
            packet[4] = 1; packet[5] = 0; packet[6] = 1;
            packet[8] = 0x01; packet[9] = 0;
            packet[10] = 64; packet[11] = 192;
            packet[12] = 96; packet[13] = 144; packet[14] = 160; packet[15] = 112;
            packet[16] = 0x34; packet[17] = 0x12;
            using (UdpClient client = new UdpClient())
                client.Send(packet, packet.Length, discoverySender);
            Thread.Sleep(150);

            using (WebClient web = new WebClient())
            {
                web.Encoding = Encoding.UTF8;
                string health = web.DownloadString("http://127.0.0.1:" + testHttpPort + "/health");
                string state = web.DownloadString("http://127.0.0.1:" + testHttpPort + "/state?pad=0");
                string overlay = web.DownloadString("http://127.0.0.1:" + testHttpPort + "/overlay.html");
                if (health != "OK") throw new Exception("health response mismatch");
                if (!state.Contains("\"lt\":64") || !state.Contains("\"rt\":192") || !state.Contains("\"connected\":true"))
                    throw new Exception("XPV3 state mismatch: " + state);
                if (!overlay.Contains("element.style.opacity=String(pressure/255)"))
                    throw new Exception("trigger pressure viewer code missing");
                Console.WriteLine("HEALTH=OK");
                Console.WriteLine("XPVD_DISCOVERY=OK");
                Console.WriteLine("SUBNET_BROADCAST_CALC=OK");
                Console.WriteLine("DYNAMIC_REPLY_PORT=OK");
                Console.WriteLine("XPV3_L2=64");
                Console.WriteLine("XPV3_R2=192");
                Console.WriteLine("HTTP_STATE=OK");
                Console.WriteLine("TRIGGER_OPACITY_HTML=OK");
                Console.WriteLine("X360_BUTTON_MAP=OK");
                Console.WriteLine("X360_TRIGGER_MAP=OK");
                Console.WriteLine("X360_AXIS_MAP=OK");
                Console.WriteLine("X360_STALE_NEUTRAL=OK");
                Console.WriteLine("VIGEM_REFLECTION_BRIDGE=OK");
            }
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.ToString());
            return 1;
        }
        finally
        {
            service.Stop();
        }
    }
}
