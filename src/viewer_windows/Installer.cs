using Microsoft.Win32;
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("PS3xPAD Viewer Installer")]
[assembly: AssemblyDescription("Instalador do PS3xPAD Viewer e controle virtual para Windows")]
[assembly: AssemblyProduct("PS3xPAD Viewer")]
[assembly: AssemblyCompany("PS3xPAD Community Build")]
[assembly: AssemblyVersion("4.0.0.0")]
[assembly: AssemblyFileVersion("4.0.0.0")]

namespace PS3xPADViewerInstaller
{
    internal sealed class PayloadFile
    {
        public readonly string Resource;
        public readonly string RelativePath;
        public PayloadFile(string resource, string relativePath)
        {
            Resource = resource;
            RelativePath = relativePath;
        }
    }

    internal static class InstallCore
    {
        public const string ProductName = "PS3xPAD Viewer";
        public const string Version = "4.0";
        public const string FirewallRule = "PS3xPAD Viewer UDP 39000";
        public const string UninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS3xPADViewer";
        private const string ClientSha256 = "4458301000B732D115521E99F9936F4EDB70D6CEB3036EF158715E0E6B8902E0";
        private const string DriverSha256 = "89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A";
        private const string DriverResource = "Payload.ViGEmBus_1.22.0_x64_x86_arm64.exe";

        private static readonly PayloadFile[] Payload = new PayloadFile[]
        {
            new PayloadFile("Payload.PS3xPADViewer.exe", "PS3xPADViewer.exe"),
            new PayloadFile("Payload.Nefarius.ViGEm.Client.dll", "Nefarius.ViGEm.Client.dll"),
            new PayloadFile("Payload.overlay.html", "overlay.html"),
            new PayloadFile("Payload.TESTAR_OVERLAY.html", "TESTAR_OVERLAY.html"),
            new PayloadFile("Payload.README_PC.txt", "README_PC.txt"),
            new PayloadFile("Payload.OBS_URLS.txt", "OBS_URLS.txt"),
            new PayloadFile("Payload.THIRD_PARTY_NOTICES.txt", "THIRD_PARTY_NOTICES.txt"),
            new PayloadFile("Payload.assets.base.png", @"assets\base.png"),
            new PayloadFile("Payload.assets.circle_pressed.png", @"assets\circle_pressed.png"),
            new PayloadFile("Payload.assets.cross_pressed.png", @"assets\cross_pressed.png"),
            new PayloadFile("Payload.assets.dpad_down_pressed.png", @"assets\dpad_down_pressed.png"),
            new PayloadFile("Payload.assets.dpad_left_pressed.png", @"assets\dpad_left_pressed.png"),
            new PayloadFile("Payload.assets.dpad_right_pressed.png", @"assets\dpad_right_pressed.png"),
            new PayloadFile("Payload.assets.dpad_up_pressed.png", @"assets\dpad_up_pressed.png"),
            new PayloadFile("Payload.assets.l1_pressed.png", @"assets\l1_pressed.png"),
            new PayloadFile("Payload.assets.l2_pressed.png", @"assets\l2_pressed.png"),
            new PayloadFile("Payload.assets.l3_pressed.png", @"assets\l3_pressed.png"),
            new PayloadFile("Payload.assets.left_analog.png", @"assets\left_analog.png"),
            new PayloadFile("Payload.assets.options_pressed.png", @"assets\options_pressed.png"),
            new PayloadFile("Payload.assets.r1_pressed.png", @"assets\r1_pressed.png"),
            new PayloadFile("Payload.assets.r2_pressed.png", @"assets\r2_pressed.png"),
            new PayloadFile("Payload.assets.r3_pressed.png", @"assets\r3_pressed.png"),
            new PayloadFile("Payload.assets.right_analog.png", @"assets\right_analog.png"),
            new PayloadFile("Payload.assets.share_pressed.png", @"assets\share_pressed.png"),
            new PayloadFile("Payload.assets.square_pressed.png", @"assets\square_pressed.png"),
            new PayloadFile("Payload.assets.triangle_pressed.png", @"assets\triangle_pressed.png")
        };

        public static string InstallDirectory
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "PS3xPAD Viewer"); }
        }

        public static string ViewerPath { get { return Path.Combine(InstallDirectory, "PS3xPADViewer.exe"); } }
        public static string UninstallerPath { get { return Path.Combine(InstallDirectory, "Uninstall.exe"); } }

        public static string Install(bool desktopShortcut, bool autoStart, bool virtualController)
        {
            KillViewer();
            Directory.CreateDirectory(InstallDirectory);
            Assembly assembly = Assembly.GetExecutingAssembly();
            for (int i = 0; i < Payload.Length; i++)
            {
                string destination = Path.Combine(InstallDirectory, Payload[i].RelativePath);
                string directory = Path.GetDirectoryName(destination);
                if (!Directory.Exists(directory)) Directory.CreateDirectory(directory);
                using (Stream input = assembly.GetManifestResourceStream(Payload[i].Resource))
                {
                    if (input == null) throw new InvalidOperationException("Recurso ausente no instalador: " + Payload[i].Resource);
                    using (FileStream output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
                        input.CopyTo(output);
                }
            }

            File.Copy(Application.ExecutablePath, UninstallerPath, true);
            CreateProgramShortcuts();
            SetDesktopShortcut(desktopShortcut);
            SetAutoStart(autoStart);
            ConfigureFirewall();
            RegisterUninstaller();
            string virtualWarning = virtualController ? InstallVirtualControllerSupport() : "";
            string virtualLog = virtualController ?
                (String.IsNullOrEmpty(virtualWarning) ? "instalado" : virtualWarning) : "não selecionado";
            File.WriteAllText(Path.Combine(InstallDirectory, "install.log"),
                "PS3xPAD Viewer " + Version + "\r\nInstalado em: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") +
                "\r\nFirewall: UDP 39000, perfil privado, rede local\r\nDescoberta automatica: UDP 39001 de saida\r\nHTTP 8765: somente 127.0.0.1\r\n" +
                "Controle virtual: " + virtualLog + "\r\n");
            return virtualWarning;
        }

        private static string InstallVirtualControllerSupport()
        {
            string warning = "";
            try
            {
                VerifyClientAssembly();
            }
            catch (Exception ex)
            {
                return "não foi possível instalar a biblioteca oficial: " + ex.Message;
            }

            if (IsViGEmBusInstalled()) return warning;

            string driver = Path.Combine(Path.GetTempPath(), "ViGEmBus_1.22.0_" + Guid.NewGuid().ToString("N") + ".exe");
            try
            {
                ExtractResource(DriverResource, driver);
                string actualHash = Sha256(driver);
                if (!String.Equals(actualHash, DriverSha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("o instalador do driver não passou na verificação SHA-256; ele não foi executado");

                ProcessStartInfo info = new ProcessStartInfo(driver);
                info.UseShellExecute = true;
                using (Process process = Process.Start(info))
                {
                    process.WaitForExit();
                    if (process.ExitCode != 0 && process.ExitCode != 3010)
                        warning = "o instalador oficial do ViGEmBus terminou com o código " + process.ExitCode;
                }
            }
            catch (Exception ex)
            {
                warning = "biblioteca instalada, mas o driver ViGEmBus não foi concluído: " + ex.Message;
            }
            finally
            {
                TryDeleteFile(driver);
            }
            return warning;
        }

        private static void VerifyClientAssembly()
        {
            string path = Path.Combine(InstallDirectory, "Nefarius.ViGEm.Client.dll");
            if (!File.Exists(path) || !String.Equals(Sha256(path), ClientSha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("a biblioteca ViGEmClient embutida não passou na verificação SHA-256");
            AssemblyName identity = AssemblyName.GetAssemblyName(path);
            if (!String.Equals(identity.Name, "Nefarius.ViGEm.Client", StringComparison.Ordinal) ||
                identity.Version == null || identity.Version.ToString() != "1.21.256.0")
                throw new InvalidDataException("a identidade da biblioteca ViGEmClient é inválida");
        }

        private static void ExtractResource(string resource, string destination)
        {
            using (Stream input = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource))
            {
                if (input == null) throw new InvalidOperationException("recurso ausente no instalador: " + resource);
                using (FileStream output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
                    input.CopyTo(output);
            }
        }

        private static string Sha256(string path)
        {
            using (SHA256 hash = SHA256.Create())
            using (FileStream stream = File.OpenRead(path))
            {
                byte[] digest = hash.ComputeHash(stream);
                StringBuilder text = new StringBuilder(digest.Length * 2);
                for (int i = 0; i < digest.Length; i++) text.Append(digest[i].ToString("X2"));
                return text.ToString();
            }
        }

        private static bool IsViGEmBusInstalled()
        {
            try
            {
                using (RegistryKey service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\ViGEmBus"))
                    if (service != null) return true;
            }
            catch { }
            return false;
        }

        private static void ExtractShortcut(string path, string target, string arguments)
        {
            string directory = Path.GetDirectoryName(path);
            if (!Directory.Exists(directory)) Directory.CreateDirectory(directory);
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            if (shellType == null) throw new InvalidOperationException("Windows Script Host não está disponível para criar atalhos.");
            object shell = Activator.CreateInstance(shellType);
            object shortcut = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { path });
            Type shortcutType = shortcut.GetType();
            shortcutType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, shortcut, new object[] { target });
            shortcutType.InvokeMember("Arguments", BindingFlags.SetProperty, null, shortcut, new object[] { arguments == null ? "" : arguments });
            shortcutType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, shortcut, new object[] { InstallDirectory });
            shortcutType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, shortcut, new object[] { ViewerPath + ",0" });
            shortcutType.InvokeMember("Save", BindingFlags.InvokeMethod, null, shortcut, null);
            if (Marshal.IsComObject(shortcut)) Marshal.FinalReleaseComObject(shortcut);
            if (Marshal.IsComObject(shell)) Marshal.FinalReleaseComObject(shell);
        }

        private static string StartMenuDirectory
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), ProductName); }
        }

        private static void CreateProgramShortcuts()
        {
            ExtractShortcut(Path.Combine(StartMenuDirectory, "PS3xPAD Viewer.lnk"), ViewerPath, "");
            ExtractShortcut(Path.Combine(StartMenuDirectory, "Desinstalar PS3xPAD Viewer.lnk"), UninstallerPath, "/uninstall");
        }

        private static void SetDesktopShortcut(bool enabled)
        {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory), "PS3xPAD Viewer.lnk");
            if (enabled) ExtractShortcut(path, ViewerPath, "");
            else TryDeleteFile(path);
        }

        private static void SetAutoStart(bool enabled)
        {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Startup), "PS3xPAD Viewer.lnk");
            if (enabled) ExtractShortcut(path, ViewerPath, "--background");
            else TryDeleteFile(path);
        }

        private static int RunNetsh(string arguments)
        {
            ProcessStartInfo info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "netsh.exe"), arguments);
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.WindowStyle = ProcessWindowStyle.Hidden;
            using (Process process = Process.Start(info))
            {
                process.WaitForExit(15000);
                return process.HasExited ? process.ExitCode : -1;
            }
        }

        private static void ConfigureFirewall()
        {
            RunNetsh("advfirewall firewall delete rule name=\"" + FirewallRule + "\"");
            int result = RunNetsh("advfirewall firewall add rule name=\"" + FirewallRule + "\" dir=in action=allow " +
                "program=\"" + ViewerPath + "\" protocol=UDP localport=39000 profile=private remoteip=localsubnet enable=yes");
            if (result != 0) throw new InvalidOperationException("O Windows Firewall recusou a criação da regra UDP 39000 (código " + result + ").");
        }

        private static void RegisterUninstaller()
        {
            using (RegistryKey key = Registry.LocalMachine.CreateSubKey(UninstallKey))
            {
                if (key == null) throw new InvalidOperationException("Não foi possível registrar o desinstalador.");
                key.SetValue("DisplayName", ProductName);
                key.SetValue("DisplayVersion", Version);
                key.SetValue("Publisher", "PS3xPAD Viewer");
                key.SetValue("InstallLocation", InstallDirectory);
                key.SetValue("DisplayIcon", ViewerPath);
                key.SetValue("UninstallString", "\"" + UninstallerPath + "\" /uninstall");
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
                key.SetValue("EstimatedSize", 4096, RegistryValueKind.DWord);
                key.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd"));
            }
        }

        public static void KillViewer()
        {
            Process[] processes = Process.GetProcessesByName("PS3xPADViewer");
            for (int i = 0; i < processes.Length; i++)
            {
                try
                {
                    processes[i].Kill();
                    processes[i].WaitForExit(3000);
                }
                catch { }
                finally { processes[i].Dispose(); }
            }
        }

        public static void Uninstall(string directory)
        {
            KillViewer();
            RunNetsh("advfirewall firewall delete rule name=\"" + FirewallRule + "\"");
            TryDeleteFile(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory), "PS3xPAD Viewer.lnk"));
            TryDeleteFile(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Startup), "PS3xPAD Viewer.lnk"));
            try { if (Directory.Exists(StartMenuDirectory)) Directory.Delete(StartMenuDirectory, true); } catch { }
            try { Registry.LocalMachine.DeleteSubKeyTree(UninstallKey, false); } catch { }
            try { if (Directory.Exists(directory)) Directory.Delete(directory, true); } catch (Exception ex) { throw new IOException("Não foi possível remover " + directory + ".", ex); }
        }

        private static void TryDeleteFile(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); } catch { }
        }
    }

    internal sealed class InstallerForm : Form
    {
        private readonly CheckBox desktop;
        private readonly CheckBox autoStart;
        private readonly CheckBox virtualController;
        private readonly Button install;
        private readonly ProgressBar progress;
        private readonly Label status;

        public InstallerForm()
        {
            Text = "Instalar PS3xPAD Viewer";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ClientSize = new Size(600, 445);
            Font = new Font("Segoe UI", 9F);

            Label title = new Label();
            title.Text = "PS3xPAD Viewer " + InstallCore.Version;
            title.Font = new Font("Segoe UI Semibold", 17F, FontStyle.Bold);
            title.AutoSize = true;
            title.Location = new Point(25, 22);
            Controls.Add(title);

            Label description = new Label();
            description.Text = "Instala o viewer nativo, a ponte UDP e, opcionalmente, um controle Xbox 360 virtual.\r\n" +
                "Nenhuma instalação de Python é necessária. Feche o viewer/BAT antigo antes de instalar.";
            description.Location = new Point(28, 67);
            description.Size = new Size(540, 45);
            Controls.Add(description);

            Label security = new Label();
            security.Text = "Firewall: libera somente UDP 39000 para este programa, no perfil Privado e a partir da rede local.\r\n" +
                "A descoberta automática usa UDP 39001 de saída. O HTTP 8765 permanece somente neste PC.";
            security.BackColor = Color.FromArgb(238, 246, 255);
            security.BorderStyle = BorderStyle.FixedSingle;
            security.Location = new Point(28, 124);
            security.Padding = new Padding(8);
            security.Size = new Size(544, 62);
            Controls.Add(security);

            desktop = new CheckBox();
            desktop.Text = "Criar atalho na área de trabalho";
            desktop.Checked = true;
            desktop.AutoSize = true;
            desktop.Location = new Point(31, 207);
            Controls.Add(desktop);

            autoStart = new CheckBox();
            autoStart.Text = "Iniciar automaticamente com o Windows (minimizado ao lado do relógio)";
            autoStart.Checked = true;
            autoStart.AutoSize = true;
            autoStart.Location = new Point(31, 235);
            Controls.Add(autoStart);

            virtualController = new CheckBox();
            virtualController.Text = "Ativar controle virtual Xbox 360 para Gamepad Viewer e outros sites";
            virtualController.Checked = true;
            virtualController.AutoSize = true;
            virtualController.Location = new Point(31, 263);
            Controls.Add(virtualController);

            Label driverNote = new Label();
            driverNote.Text = "Inclui o ViGEmBus 1.22.0 oficial. O instalador do driver será mostrado para sua confirmação.\r\n" +
                "O projeto foi encerrado; esta versão final não contém o atualizador antigo.";
            driverNote.ForeColor = Color.DimGray;
            driverNote.Location = new Point(50, 290);
            driverNote.Size = new Size(510, 45);
            Controls.Add(driverNote);

            progress = new ProgressBar();
            progress.Location = new Point(29, 350);
            progress.Size = new Size(430, 24);
            progress.Style = ProgressBarStyle.Continuous;
            Controls.Add(progress);

            install = new Button();
            install.Text = "Instalar";
            install.Location = new Point(472, 346);
            install.Size = new Size(100, 34);
            install.Click += InstallClicked;
            Controls.Add(install);

            status = new Label();
            status.Text = "Destino: " + InstallCore.InstallDirectory;
            status.AutoEllipsis = true;
            status.Location = new Point(29, 397);
            status.Size = new Size(543, 32);
            Controls.Add(status);
        }

        private void InstallClicked(object sender, EventArgs e)
        {
            install.Enabled = false;
            desktop.Enabled = false;
            autoStart.Enabled = false;
            virtualController.Enabled = false;
            progress.Style = ProgressBarStyle.Marquee;
            status.Text = "Instalando aplicativo, Firewall e suporte ao controle virtual...";
            Application.DoEvents();
            try
            {
                string warning = InstallCore.Install(desktop.Checked, autoStart.Checked, virtualController.Checked);
                progress.Style = ProgressBarStyle.Continuous;
                progress.Value = 100;
                status.Text = "Instalação concluída.";
                string message = "PS3xPAD Viewer instalado com sucesso.\r\n\r\nA regra UDP 39000 foi criada e o viewer será aberto agora.";
                MessageBoxIcon icon = MessageBoxIcon.Information;
                if (!String.IsNullOrEmpty(warning))
                {
                    message += "\r\n\r\nAviso sobre o controle virtual: " + warning;
                    icon = MessageBoxIcon.Warning;
                }
                MessageBox.Show(message, "PS3xPAD Viewer", MessageBoxButtons.OK, icon);
                ProcessStartInfo info = new ProcessStartInfo(InstallCore.ViewerPath);
                info.UseShellExecute = true;
                Process.Start(info);
                Close();
            }
            catch (Exception ex)
            {
                progress.Style = ProgressBarStyle.Continuous;
                progress.Value = 0;
                status.Text = "Falha na instalação.";
                MessageBox.Show("A instalação não foi concluída.\r\n\r\n" + ex.Message,
                    "PS3xPAD Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                install.Enabled = true;
                desktop.Enabled = true;
                autoStart.Enabled = true;
                virtualController.Enabled = true;
            }
        }
    }

    internal static class Program
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool MoveFileEx(string existingFile, string newFile, int flags);
        private const int MoveFileDelayUntilReboot = 0x4;

        [STAThread]
        private static void Main(string[] args)
        {
            if (args.Length > 0 && String.Equals(args[0], "/uninstall-worker", StringComparison.OrdinalIgnoreCase))
            {
                RunUninstallWorker(args);
                return;
            }
            if (args.Length > 0 && String.Equals(args[0], "/uninstall", StringComparison.OrdinalIgnoreCase))
            {
                BeginUninstall();
                return;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new InstallerForm());
        }

        private static void BeginUninstall()
        {
            Application.EnableVisualStyles();
            DialogResult answer = MessageBox.Show("Deseja remover o PS3xPAD Viewer e sua regra de Firewall?\r\n\r\n" +
                "O driver ViGEmBus será mantido, pois outros programas podem utilizá-lo.",
                "Desinstalar PS3xPAD Viewer", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (answer != DialogResult.Yes) return;
            try
            {
                string temp = Path.Combine(Path.GetTempPath(), "PS3xPADViewer_Uninstall_" + Guid.NewGuid().ToString("N") + ".exe");
                File.Copy(Application.ExecutablePath, temp, true);
                ProcessStartInfo info = new ProcessStartInfo(temp);
                info.Arguments = "/uninstall-worker \"" + InstallCore.InstallDirectory + "\" " + Process.GetCurrentProcess().Id;
                info.UseShellExecute = true;
                Process.Start(info);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Não foi possível iniciar a desinstalação.\r\n\r\n" + ex.Message,
                    "PS3xPAD Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private static void RunUninstallWorker(string[] args)
        {
            Application.EnableVisualStyles();
            string directory = args.Length > 1 ? args[1] : InstallCore.InstallDirectory;
            int processId;
            if (args.Length > 2 && Int32.TryParse(args[2], out processId))
            {
                try { Process.GetProcessById(processId).WaitForExit(10000); } catch { }
            }
            try
            {
                Thread.Sleep(500);
                InstallCore.Uninstall(directory);
                MoveFileEx(Application.ExecutablePath, null, MoveFileDelayUntilReboot);
                MessageBox.Show("PS3xPAD Viewer removido. A regra de Firewall UDP 39000 também foi excluída.\r\n\r\n" +
                    "O driver compartilhado ViGEmBus foi mantido.",
                    "PS3xPAD Viewer", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show("A desinstalação não foi concluída.\r\n\r\n" + ex.Message,
                    "PS3xPAD Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }
}
