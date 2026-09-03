using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;

namespace SkyrimMPLauncher;

internal sealed class LauncherConfig
{
    public string SkyrimPath { get; set; } = @"C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition";
    public string ServerAddress { get; set; } = "127.0.0.1";
    public int ServerPort { get; set; } = 10578;
}

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new LauncherForm());
    }
}

internal sealed class LauncherForm : Form
{
    private readonly TextBox _skyrimPath = new() { Dock = DockStyle.Fill };
    private readonly TextBox _serverAddress = new() { Dock = DockStyle.Fill };
    private readonly NumericUpDown _serverPort = new() { Minimum = 1, Maximum = 65535, Value = 10578, Dock = DockStyle.Fill };
    private readonly Label _status = new() { AutoSize = true, Text = "Ready." };
    private readonly string _configPath;
    private readonly CancellationTokenSource _shutdown = new();
    private Task? _relayTask;

    public LauncherForm()
    {
        Text = "SkyrimMP Launcher";
        Width = 690;
        Height = 320;
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;

        var appDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SkyrimMP");
        Directory.CreateDirectory(appDir);
        _configPath = Path.Combine(appDir, "launcher.json");

        var browse = new Button { Text = "Browse...", AutoSize = true };
        browse.Click += (_, _) => BrowseSkyrim();

        var single = new Button { Text = "Play Single Player", AutoSize = true, Height = 42 };
        single.Click += async (_, _) => await LaunchAsync(multiplayer: false);

        var multi = new Button { Text = "Play Multiplayer", AutoSize = true, Height = 42 };
        multi.Click += async (_, _) => await LaunchAsync(multiplayer: true);

        var grid = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(14),
            ColumnCount = 3,
            RowCount = 6
        };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        grid.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        grid.Controls.Add(new Label { Text = "Skyrim folder", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0);
        grid.Controls.Add(_skyrimPath, 1, 0);
        grid.Controls.Add(browse, 2, 0);
        grid.Controls.Add(new Label { Text = "Server IPv4", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
        grid.Controls.Add(_serverAddress, 1, 1);
        grid.SetColumnSpan(_serverAddress, 2);
        grid.Controls.Add(new Label { Text = "UDP port", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2);
        grid.Controls.Add(_serverPort, 1, 2);

        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.LeftToRight };
        buttons.Controls.Add(single);
        buttons.Controls.Add(multi);
        grid.Controls.Add(buttons, 1, 3);
        grid.SetColumnSpan(buttons, 2);
        grid.Controls.Add(_status, 1, 4);
        grid.SetColumnSpan(_status, 2);

        Controls.Add(grid);
        LoadConfig();
        FormClosing += (_, _) => _shutdown.Cancel();
    }

    private void BrowseSkyrim()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select the Skyrim Special Edition folder",
            UseDescriptionForTitle = true,
            SelectedPath = Directory.Exists(_skyrimPath.Text) ? _skyrimPath.Text : string.Empty
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
            _skyrimPath.Text = dialog.SelectedPath;
    }

    private LauncherConfig CurrentConfig() => new()
    {
        SkyrimPath = _skyrimPath.Text.Trim(),
        ServerAddress = _serverAddress.Text.Trim(),
        ServerPort = (int)_serverPort.Value
    };

    private void LoadConfig()
    {
        LauncherConfig config = new();
        try
        {
            if (File.Exists(_configPath))
                config = JsonSerializer.Deserialize<LauncherConfig>(File.ReadAllText(_configPath)) ?? config;
        }
        catch { }
        _skyrimPath.Text = config.SkyrimPath;
        _serverAddress.Text = config.ServerAddress;
        _serverPort.Value = Math.Clamp(config.ServerPort, 1, 65535);
    }

    private void SaveConfig(LauncherConfig config)
    {
        File.WriteAllText(_configPath, JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true }));
    }

    private async Task LaunchAsync(bool multiplayer)
    {
        try
        {
            var config = CurrentConfig();
            SaveConfig(config);
            ValidateSkyrim(config.SkyrimPath);
            InstallBundledClientIfPresent(config.SkyrimPath);

            var pluginDir = Path.Combine(config.SkyrimPath, "Data", "SKSE", "Plugins");
            Directory.CreateDirectory(pluginDir);
            var enabled = Path.Combine(pluginDir, "SkyrimMultiplayer.dll");
            var disabled = Path.Combine(pluginDir, "SkyrimMultiplayer.dll.disabled");

            if (multiplayer)
            {
                if (!File.Exists(enabled) && File.Exists(disabled)) File.Move(disabled, enabled, true);
                if (!File.Exists(enabled)) throw new InvalidOperationException("SkyrimMultiplayer.dll is not installed.");

                if (!IPAddress.TryParse(config.ServerAddress, out var ip) || ip.AddressFamily != AddressFamily.InterNetwork)
                    throw new InvalidOperationException("Enter a valid IPv4 address for the SkyrimMP server.");

                _shutdown.CancelAfter(Timeout.InfiniteTimeSpan);
                _relayTask = RunRelayAsync(ip, config.ServerPort, _shutdown.Token);
                _status.Text = $"Multiplayer relay active: 127.0.0.1:10578 -> {ip}:{config.ServerPort}";
            }
            else
            {
                if (File.Exists(enabled)) File.Move(enabled, disabled, true);
                _status.Text = "Multiplayer plugin disabled for this launch.";
            }

            var skse = Path.Combine(config.SkyrimPath, "skse64_loader.exe");
            var process = Process.Start(new ProcessStartInfo
            {
                FileName = skse,
                WorkingDirectory = config.SkyrimPath,
                UseShellExecute = true
            }) ?? throw new InvalidOperationException("Failed to start SKSE64.");

            _status.Text += " Skyrim started.";
            await Task.Run(() => process.WaitForExit());
            _status.Text = multiplayer ? "Skyrim closed. Close this launcher when finished." : "Skyrim closed.";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "SkyrimMP Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
            _status.Text = "Launch failed.";
        }
    }

    private static void ValidateSkyrim(string path)
    {
        if (!File.Exists(Path.Combine(path, "SkyrimSE.exe")))
            throw new InvalidOperationException("SkyrimSE.exe was not found in the selected folder.");
        if (!File.Exists(Path.Combine(path, "skse64_loader.exe")))
            throw new InvalidOperationException("skse64_loader.exe was not found. Install the correct SKSE64 build first.");
    }

    private static void InstallBundledClientIfPresent(string skyrimPath)
    {
        var bundled = Path.Combine(AppContext.BaseDirectory, "SkyrimMultiplayer.dll");
        if (!File.Exists(bundled)) return;
        var pluginDir = Path.Combine(skyrimPath, "Data", "SKSE", "Plugins");
        Directory.CreateDirectory(pluginDir);
        File.Copy(bundled, Path.Combine(pluginDir, "SkyrimMultiplayer.dll"), true);
    }

    private static async Task RunRelayAsync(IPAddress serverAddress, int serverPort, CancellationToken token)
    {
        using var local = new UdpClient(new IPEndPoint(IPAddress.Loopback, 10578));
        using var upstream = new UdpClient();
        var server = new IPEndPoint(serverAddress, serverPort);
        IPEndPoint? client = null;

        while (!token.IsCancellationRequested)
        {
            var localReceive = local.ReceiveAsync(token).AsTask();
            var upstreamReceive = upstream.ReceiveAsync(token).AsTask();
            var completed = await Task.WhenAny(localReceive, upstreamReceive);

            if (completed == localReceive)
            {
                var packet = await localReceive;
                client = packet.RemoteEndPoint;
                await upstream.SendAsync(packet.Buffer, server, token);
            }
            else
            {
                var packet = await upstreamReceive;
                if (client is not null)
                    await local.SendAsync(packet.Buffer, client, token);
            }
        }
    }
}
