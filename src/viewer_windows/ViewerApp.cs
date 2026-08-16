using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("XPAD Revolution Viewer")]
[assembly: AssemblyDescription("Viewer UDP/HTTP e controle Xbox 360 virtual para o XPAD Revolution")]
[assembly: AssemblyProduct("XPAD Revolution")]
[assembly: AssemblyCompany("XPAD Revolution Community")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace PS3xPADViewer
{
    internal sealed class PadState
    {
        public int Pad;
        public bool Connected;
        public int Buttons;
        public int L2;
        public int R2;
        public int LX = 128;
        public int LY = 128;
        public int RX = 128;
        public int RY = 128;
        public int Sequence;
        public string Ps3Ip = "";
        public DateTime LastSeenUtc = DateTime.MinValue;
        public int Version;

        public PadState Clone()
        {
            return (PadState)MemberwiseClone();
        }
    }

    internal sealed class ViewerService : IDisposable
    {
        public const int UdpPort = 39000;
        public const int HttpPort = 8765;
        public const int DiscoveryPort = 39001;
        private const int MaxPads = 7;
        private static readonly byte[] DiscoveryPacket = new byte[]
        {
            (byte)'X', (byte)'P', (byte)'V', (byte)'D', 1, 0, 0, 0
        };
        private readonly object stateLock = new object();
        private readonly PadState[] states = new PadState[MaxPads];
        private readonly VirtualGamepadBridge virtualGamepad = new VirtualGamepadBridge();
        private readonly string contentRoot;
        private readonly int udpPort;
        private readonly int httpPort;
        private volatile bool running;
        private UdpClient udp;
        private TcpListener http;
        private Thread udpThread;
        private Thread httpThread;

        public ViewerService(string root) : this(root, UdpPort, HttpPort)
        {
        }

        internal ViewerService(string root, int requestedUdpPort, int requestedHttpPort)
        {
            contentRoot = root;
            udpPort = requestedUdpPort;
            httpPort = requestedHttpPort;
            for (int i = 0; i < states.Length; i++)
            {
                states[i] = new PadState();
                states[i].Pad = i;
            }
        }

        public bool IsRunning { get { return running; } }

        public void Start()
        {
            if (running) return;
            try
            {
                try
                {
                    udp = new UdpClient(new IPEndPoint(IPAddress.Any, udpPort));
                    udp.EnableBroadcast = true;
                }
                catch (Exception ex)
                {
                    throw new InvalidOperationException("Não foi possível abrir a porta UDP " + udpPort + ". Feche o viewer antigo e tente novamente.", ex);
                }
                udp.Client.ReceiveTimeout = 250;
                try
                {
                    http = new TcpListener(IPAddress.Loopback, httpPort);
                    http.Start();
                }
                catch (Exception ex)
                {
                    throw new InvalidOperationException("Não foi possível abrir a porta HTTP local " + httpPort + ". Feche o viewer antigo e tente novamente.", ex);
                }
                running = true;
                udpThread = new Thread(UdpLoop);
                udpThread.IsBackground = true;
                udpThread.Name = "PS3xPAD UDP";
                httpThread = new Thread(HttpLoop);
                httpThread.IsBackground = true;
                httpThread.Name = "PS3xPAD HTTP";
                udpThread.Start();
                httpThread.Start();
                virtualGamepad.Start(contentRoot);
            }
            catch
            {
                Stop();
                throw;
            }
        }

        public void Stop()
        {
            running = false;
            try { if (udp != null) udp.Close(); } catch { }
            try { if (http != null) http.Stop(); } catch { }
            try { virtualGamepad.Dispose(); } catch { }
            if (udpThread != null && udpThread.IsAlive) udpThread.Join(1000);
            if (httpThread != null && httpThread.IsAlive) httpThread.Join(1000);
            udpThread = null;
            httpThread = null;
            udp = null;
            http = null;
        }

        private void UdpLoop()
        {
            DateTime nextDiscoveryUtc = DateTime.MinValue;
            while (running)
            {
                if (DateTime.UtcNow >= nextDiscoveryUtc)
                {
                    BroadcastDiscovery();
                    nextDiscoveryUtc = DateTime.UtcNow.AddSeconds(1);
                }
                try
                {
                    IPEndPoint sender = new IPEndPoint(IPAddress.Any, 0);
                    byte[] data = udp.Receive(ref sender);
                    UpdatePacket(data, sender.Address.ToString());
                }
                catch (SocketException ex)
                {
                    if (!running) break;
                    if (ex.SocketErrorCode != SocketError.TimedOut) Thread.Sleep(100);
                }
                catch (ObjectDisposedException) { break; }
                catch { Thread.Sleep(100); }
                ExpirePads();
            }
        }

        internal static IPAddress CalculateBroadcast(IPAddress address, IPAddress mask)
        {
            byte[] ip = address.GetAddressBytes();
            byte[] netmask = mask.GetAddressBytes();
            if (ip.Length != 4 || netmask.Length != 4) return null;
            byte[] broadcast = new byte[4];
            for (int i = 0; i < 4; i++)
                broadcast[i] = (byte)(ip[i] | (byte)~netmask[i]);
            return new IPAddress(broadcast);
        }

        internal void SendDiscoveryTo(IPEndPoint destination)
        {
            UdpClient socket = udp;
            if (!running || socket == null || destination == null) return;
            socket.Send(DiscoveryPacket, DiscoveryPacket.Length, destination);
        }

        private void BroadcastDiscovery()
        {
            HashSet<string> sent = new HashSet<string>(StringComparer.Ordinal);
            try
            {
                IPEndPoint limited = new IPEndPoint(IPAddress.Broadcast, DiscoveryPort);
                SendDiscoveryTo(limited);
                sent.Add(limited.Address.ToString());
            }
            catch { }

            try
            {
                foreach (NetworkInterface adapter in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (adapter.OperationalStatus != OperationalStatus.Up ||
                        adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback ||
                        adapter.NetworkInterfaceType == NetworkInterfaceType.Tunnel)
                        continue;
                    foreach (UnicastIPAddressInformation unicast in adapter.GetIPProperties().UnicastAddresses)
                    {
                        if (unicast.Address.AddressFamily != AddressFamily.InterNetwork || unicast.IPv4Mask == null)
                            continue;
                        IPAddress address = CalculateBroadcast(unicast.Address, unicast.IPv4Mask);
                        if (address == null || !sent.Add(address.ToString())) continue;
                        try { SendDiscoveryTo(new IPEndPoint(address, DiscoveryPort)); } catch { }
                    }
                }
            }
            catch { }
        }

        private void UpdatePacket(byte[] data, string ip)
        {
            if (data == null || data.Length != 18) return;
            if (data[0] != (byte)'X' || data[1] != (byte)'P' || data[2] != (byte)'V' || data[3] != (byte)'3') return;
            if (data[4] != 1 || data[5] >= MaxPads) return;
            int pad = data[5];
            PadState updated;
            lock (stateLock)
            {
                PadState old = states[pad];
                bool connected = (data[6] & 1) != 0;
                int buttons = data[8] | (data[9] << 8);
                bool changed = old.Connected != connected || old.Buttons != buttons ||
                    old.L2 != data[10] || old.R2 != data[11] || old.LX != data[12] ||
                    old.LY != data[13] || old.RX != data[14] || old.RY != data[15] || old.Ps3Ip != ip;
                old.Connected = connected;
                old.Buttons = buttons;
                old.L2 = data[10];
                old.R2 = data[11];
                old.LX = data[12];
                old.LY = data[13];
                old.RX = data[14];
                old.RY = data[15];
                old.Sequence = data[16] | (data[17] << 8);
                old.Ps3Ip = ip;
                old.LastSeenUtc = DateTime.UtcNow;
                if (changed) old.Version++;
                updated = old.Clone();
            }
            virtualGamepad.Update(updated);
        }

        private void ExpirePads()
        {
            DateTime now = DateTime.UtcNow;
            PadState expiredPadZero = null;
            lock (stateLock)
            {
                for (int i = 0; i < states.Length; i++)
                {
                    PadState s = states[i];
                    if (s.Connected && s.LastSeenUtc != DateTime.MinValue && (now - s.LastSeenUtc).TotalSeconds > 1.0)
                    {
                        s.Connected = false;
                        s.Buttons = 0;
                        s.L2 = s.R2 = 0;
                        s.LX = s.LY = s.RX = s.RY = 128;
                        s.Version++;
                        if (i == 0) expiredPadZero = s.Clone();
                    }
                }
            }
            if (expiredPadZero != null) virtualGamepad.Update(expiredPadZero);
        }

        public PadState Snapshot(int pad)
        {
            if (pad < 0) pad = 0;
            if (pad >= MaxPads) pad = MaxPads - 1;
            lock (stateLock) return states[pad].Clone();
        }

        public string StatusText()
        {
            string network = "Procurando o PS3 automaticamente na rede local...";
            lock (stateLock)
            {
                for (int i = 0; i < states.Length; i++)
                {
                    if (states[i].Connected)
                    {
                        network = String.Format(CultureInfo.InvariantCulture,
                            "PS3 detectado: {0}  |  Pad {1}  |  L2 {2}/255  |  R2 {3}/255",
                            states[i].Ps3Ip, i + 1, states[i].L2, states[i].R2);
                        break;
                    }
                }
            }
            return network + "\r\n" + virtualGamepad.StatusText;
        }

        private void HttpLoop()
        {
            while (running)
            {
                try
                {
                    TcpClient client = http.AcceptTcpClient();
                    ThreadPool.QueueUserWorkItem(delegate { HandleClient(client); });
                }
                catch (SocketException) { if (running) Thread.Sleep(50); }
                catch (ObjectDisposedException) { break; }
                catch { if (running) Thread.Sleep(50); }
            }
        }

        private void HandleClient(TcpClient client)
        {
            using (client)
            {
                try
                {
                    client.ReceiveTimeout = 5000;
                    client.SendTimeout = 5000;
                    NetworkStream stream = client.GetStream();
                    StreamReader reader = new StreamReader(stream, Encoding.ASCII, false, 1024, true);
                    string request = reader.ReadLine();
                    if (String.IsNullOrEmpty(request)) return;
                    string line;
                    do { line = reader.ReadLine(); } while (!String.IsNullOrEmpty(line));
                    string[] parts = request.Split(' ');
                    if (parts.Length < 2 || parts[0] != "GET")
                    {
                        WriteText(stream, 405, "text/plain; charset=utf-8", "Method Not Allowed");
                        return;
                    }
                    string target = parts[1];
                    int q = target.IndexOf('?');
                    string path = q >= 0 ? target.Substring(0, q) : target;
                    string query = q >= 0 ? target.Substring(q + 1) : "";
                    path = Uri.UnescapeDataString(path);
                    int pad = QueryPad(query);
                    if (path == "/events")
                    {
                        ServeEvents(stream, pad);
                        return;
                    }
                    if (path == "/state")
                    {
                        WriteText(stream, 200, "application/json; charset=utf-8", ToJson(Snapshot(pad), true));
                        return;
                    }
                    if (path == "/health")
                    {
                        WriteText(stream, 200, "text/plain; charset=utf-8", "OK");
                        return;
                    }
                    if (path == "/" || path == "/overlay.html")
                    {
                        ServeFile(stream, Path.Combine(contentRoot, "overlay.html"));
                        return;
                    }
                    if (String.Equals(path, "/TESTAR_OVERLAY.html", StringComparison.OrdinalIgnoreCase))
                    {
                        ServeFile(stream, Path.Combine(contentRoot, "TESTAR_OVERLAY.html"));
                        return;
                    }
                    if (path.StartsWith("/assets/", StringComparison.OrdinalIgnoreCase))
                    {
                        string requested = path.Substring("/assets/".Length);
                        if (requested != Path.GetFileName(requested))
                        {
                            WriteText(stream, 404, "text/plain", "404");
                            return;
                        }
                        ServeFile(stream, Path.Combine(contentRoot, "assets", requested));
                        return;
                    }
                    WriteText(stream, 404, "text/plain", "404");
                }
                catch (IOException) { }
                catch (SocketException) { }
                catch (ObjectDisposedException) { }
                catch { }
            }
        }

        private static int QueryPad(string query)
        {
            string[] fields = query.Split('&');
            for (int i = 0; i < fields.Length; i++)
            {
                string[] pair = fields[i].Split('=');
                int value;
                if (pair.Length == 2 && pair[0] == "pad" && Int32.TryParse(pair[1], out value))
                    return Math.Max(0, Math.Min(6, value));
            }
            return 0;
        }

        private void ServeEvents(NetworkStream stream, int pad)
        {
            WriteBytes(stream, Encoding.ASCII.GetBytes(
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n" +
                "Connection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n"));
            int lastVersion = -1;
            DateTime lastPing = DateTime.MinValue;
            while (running)
            {
                PadState state = Snapshot(pad);
                DateTime now = DateTime.UtcNow;
                string message = null;
                if (state.Version != lastVersion)
                {
                    message = "data: " + ToJson(state, false) + "\n\n";
                    lastVersion = state.Version;
                    lastPing = now;
                }
                else if ((now - lastPing).TotalSeconds >= 1.0)
                {
                    message = ": ping\n\n";
                    lastPing = now;
                }
                if (message != null)
                {
                    WriteBytes(stream, new UTF8Encoding(false).GetBytes(message));
                    stream.Flush();
                }
                Thread.Sleep(8);
            }
        }

        private static string ToJson(PadState s, bool includeAge)
        {
            long age = s.LastSeenUtc == DateTime.MinValue ? -1 : (long)Math.Max(0, (DateTime.UtcNow - s.LastSeenUtc).TotalMilliseconds);
            string json = String.Format(CultureInfo.InvariantCulture,
                "{{\"pad\":{0},\"connected\":{1},\"buttons\":{2},\"lt\":{3},\"rt\":{4}," +
                "\"lx\":{5},\"ly\":{6},\"rx\":{7},\"ry\":{8},\"sequence\":{9},\"ps3_ip\":\"{10}\",\"version\":{11}",
                s.Pad, s.Connected ? "true" : "false", s.Buttons, s.L2, s.R2,
                s.LX, s.LY, s.RX, s.RY, s.Sequence, s.Ps3Ip.Replace("\\", "\\\\").Replace("\"", "\\\""), s.Version);
            if (includeAge) json += ",\"age_ms\":" + (age < 0 ? "null" : age.ToString(CultureInfo.InvariantCulture));
            return json + "}";
        }

        private static void ServeFile(NetworkStream stream, string path)
        {
            if (!File.Exists(path))
            {
                WriteText(stream, 404, "text/plain", "404");
                return;
            }
            string ext = Path.GetExtension(path).ToLowerInvariant();
            string type = ext == ".html" ? "text/html; charset=utf-8" : ext == ".png" ? "image/png" : "application/octet-stream";
            WriteResponse(stream, 200, type, File.ReadAllBytes(path));
        }

        private static void WriteText(NetworkStream stream, int status, string type, string text)
        {
            WriteResponse(stream, status, type, new UTF8Encoding(false).GetBytes(text));
        }

        private static void WriteResponse(NetworkStream stream, int status, string type, byte[] body)
        {
            string reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Error";
            byte[] header = Encoding.ASCII.GetBytes(String.Format(CultureInfo.InvariantCulture,
                "HTTP/1.1 {0} {1}\r\nContent-Type: {2}\r\nContent-Length: {3}\r\n" +
                "Cache-Control: no-store, no-cache, must-revalidate\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                status, reason, type, body.Length));
            WriteBytes(stream, header);
            WriteBytes(stream, body);
        }

        private static void WriteBytes(NetworkStream stream, byte[] data)
        {
            stream.Write(data, 0, data.Length);
        }

        public void Dispose() { Stop(); }
    }

    internal sealed class MainForm : Form
    {
        private readonly ViewerService service;
        private readonly bool backgroundStart;
        private readonly Label statusLabel;
        private readonly NotifyIcon tray;
        private readonly System.Windows.Forms.Timer timer;
        private bool exiting;
        private bool trayHintShown;

        public MainForm(bool background)
        {
            backgroundStart = background;
            Text = "XPAD Revolution Viewer";
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(720, 310);
            MinimumSize = new Size(740, 349);
            Font = new Font("Segoe UI", 9F);

            Label title = new Label();
            title.Text = "XPAD Revolution Viewer para Windows";
            title.Font = new Font("Segoe UI Semibold", 16F, FontStyle.Bold);
            title.AutoSize = true;
            title.Location = new Point(22, 18);
            Controls.Add(title);

            Label endpoints = new Label();
            endpoints.Text = "Descoberta automática  •  UDP 39000  •  http://127.0.0.1:8765/";
            endpoints.AutoSize = true;
            endpoints.ForeColor = Color.DimGray;
            endpoints.Location = new Point(25, 57);
            Controls.Add(endpoints);

            statusLabel = new Label();
            statusLabel.Text = "Iniciando...";
            statusLabel.BorderStyle = BorderStyle.FixedSingle;
            statusLabel.BackColor = Color.FromArgb(245, 248, 252);
            statusLabel.Location = new Point(25, 90);
            statusLabel.Size = new Size(670, 62);
            statusLabel.TextAlign = ContentAlignment.MiddleCenter;
            Controls.Add(statusLabel);

            Button open = new Button();
            open.Text = "Viewer XPAD Revolution";
            open.Location = new Point(25, 172);
            open.Size = new Size(150, 38);
            open.Click += delegate { OpenViewer(); };
            Controls.Add(open);

            Button standard = new Button();
            standard.Text = "Gamepad Viewer padrão";
            standard.Location = new Point(185, 172);
            standard.Size = new Size(175, 38);
            standard.Click += delegate { OpenStandardViewer(); };
            Controls.Add(standard);

            Button debug = new Button();
            debug.Text = "Pressão/debug";
            debug.Location = new Point(370, 172);
            debug.Size = new Size(150, 38);
            debug.Click += delegate { OpenViewerDebug(); };
            Controls.Add(debug);

            Button exit = new Button();
            exit.Text = "Parar e sair";
            exit.Location = new Point(530, 172);
            exit.Size = new Size(165, 38);
            exit.Click += delegate { ExitApplication(); };
            Controls.Add(exit);

            Label note = new Label();
            note.Text = "No Gamepad Viewer padrão, pressione um botão para o navegador reconhecer o controle virtual.\r\n" +
                "Fechar esta janela minimiza o aplicativo para a área de notificação.";
            note.AutoSize = true;
            note.ForeColor = Color.DimGray;
            note.Location = new Point(25, 230);
            Controls.Add(note);

            ContextMenuStrip menu = new ContextMenuStrip();
            menu.Items.Add("Abrir viewer XPAD Revolution", null, delegate { OpenViewer(); });
            menu.Items.Add("Abrir Gamepad Viewer padrão", null, delegate { OpenStandardViewer(); });
            menu.Items.Add("Mostrar janela", null, delegate { ShowWindow(); });
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Parar e sair", null, delegate { ExitApplication(); });
            tray = new NotifyIcon();
            tray.Icon = SystemIcons.Application;
            tray.Text = "XPAD Revolution";
            tray.ContextMenuStrip = menu;
            tray.Visible = true;
            tray.DoubleClick += delegate { ShowWindow(); };

            service = new ViewerService(Application.StartupPath);
            timer = new System.Windows.Forms.Timer();
            timer.Interval = 250;
            timer.Tick += delegate { statusLabel.Text = service.StatusText(); };
            Shown += OnShown;
            FormClosing += OnFormClosing;
        }

        private void OnShown(object sender, EventArgs e)
        {
            try
            {
                service.Start();
                timer.Start();
                statusLabel.Text = service.StatusText();
                if (backgroundStart)
                {
                    BeginInvoke(new MethodInvoker(delegate { Hide(); }));
                }
                else
                {
                    BeginInvoke(new MethodInvoker(delegate { OpenViewer(); }));
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Não foi possível iniciar o viewer. Verifique se as portas UDP 39000 e HTTP 8765 já estão sendo usadas.\r\n\r\n" + ex.Message,
                    "XPAD Revolution", MessageBoxButtons.OK, MessageBoxIcon.Error);
                ExitApplication();
            }
        }

        private static void OpenUrl(string url)
        {
            ProcessStartInfo info = new ProcessStartInfo(url);
            info.UseShellExecute = true;
            Process.Start(info);
        }

        private void OpenViewer() { OpenUrl("http://127.0.0.1:8765/?pad=0"); }
        private void OpenViewerDebug() { OpenUrl("http://127.0.0.1:8765/?pad=0&debug=1"); }
        private void OpenStandardViewer() { OpenUrl("https://gamepadviewer.com/"); }

        private void ShowWindow()
        {
            Show();
            WindowState = FormWindowState.Normal;
            Activate();
        }

        private void ExitApplication()
        {
            exiting = true;
            timer.Stop();
            service.Stop();
            tray.Visible = false;
            Close();
        }

        private void OnFormClosing(object sender, FormClosingEventArgs e)
        {
            if (!exiting && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                Hide();
                if (!trayHintShown)
                {
                    trayHintShown = true;
                    tray.ShowBalloonTip(2500, "XPAD Revolution", "O viewer continua ativo. Use o ícone ao lado do relógio para abrir ou sair.", ToolTipIcon.Info);
                }
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                try { service.Dispose(); } catch { }
                try { tray.Dispose(); } catch { }
                try { timer.Dispose(); } catch { }
            }
            base.Dispose(disposing);
        }
    }

    internal static class Program
    {
        private static Mutex singleInstance;

        [STAThread]
        private static void Main(string[] args)
        {
            bool created;
            singleInstance = new Mutex(true, "Local\\XPADRevolutionViewer_8C63224D", out created);
            if (!created)
            {
                try
                {
                    ProcessStartInfo info = new ProcessStartInfo("http://127.0.0.1:8765/?pad=0");
                    info.UseShellExecute = true;
                    Process.Start(info);
                }
                catch { }
                return;
            }
            bool background = false;
            for (int i = 0; i < args.Length; i++)
                if (String.Equals(args[i], "--background", StringComparison.OrdinalIgnoreCase)) background = true;

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm(background));
            try { singleInstance.ReleaseMutex(); } catch { }
            singleInstance.Dispose();
        }
    }
}
