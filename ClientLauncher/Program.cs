using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;

namespace SkyrimMPLauncher;

internal sealed class LauncherConfig
{
    public string SkyrimPath { get; set; } = string.Empty;
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
    private readonly TextBox _skyrimPath = new() { Dock = DockStyle.Fill, ReadOnly = true };
    private readonly TextBox _serverAddress = new() { Dock = DockStyle.Fill };
    private readonly NumericUpDown _serverPort = new() { Minimum = 1, Maximum = 65535, Value = 10578, Dock = DockStyle.Fill };
    private readonly Label _status = new() { AutoSize = true, Text = "Choose this PC's Skyrim Special Edition folder." };
    private readonly string _configPath;
    private readonly CancellationTokenSource _shutdown = new();
    private bool _launching;

    public LauncherForm()
    {
        Text = $"SkyrimMP Launcher {BuildInfo.Version}";
        Width = 720;
        Height = 330;
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;

        var appDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SkyrimMP");
        Directory.CreateDirectory(appDir);
        _configPath = Path.Combine(appDir, "launcher.json");

        var browse = new Button { Text = "Choose Skyrim Folder...", AutoSize = true };
        browse.Click += (_, _) => BrowseSkyrim();

        var single = new Button { Text = "Play Single Player", AutoSize = true, Height = 42 };
        single.Click += async (_, _) => await LaunchAsync(multiplayer: false);

        var multi = new Button { Text = "Join Multiplayer Server", AutoSize = true, Height = 42 };
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
        Shown += (_, _) => EnsureSkyrimFolderSelected();
        FormClosing += (_, _) => _shutdown.Cancel();
    }

    private bool BrowseSkyrim()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select THIS PC's Skyrim Special Edition install folder. It must contain SkyrimSE.exe and skse64_loader.exe.",
            UseDescriptionForTitle = true,
            SelectedPath = Directory.Exists(_skyrimPath.Text) ? _skyrimPath.Text : string.Empty,
            ShowNewFolderButton = false
        };

        while (dialog.ShowDialog(this) == DialogResult.OK)
        {
            var selected = dialog.SelectedPath;
            if (IsValidSkyrimFolder(selected, out var problem))
            {
                _skyrimPath.Text = selected;
                var config = CurrentConfig();
                SaveConfig(config);
                _status.Text = $"Skyrim install selected: {selected}";
                return true;
            }

            MessageBox.Show(this,
                $"That is not a usable Skyrim Special Edition + SKSE folder.\n\n{problem}\n\nChoose the folder that contains SkyrimSE.exe and skse64_loader.exe.",
                "Select Skyrim Install",
                MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
        }

        return false;
    }

    private void EnsureSkyrimFolderSelected()
    {
        if (IsValidSkyrimFolder(_skyrimPath.Text, out _))
        {
            _status.Text = $"Skyrim install: {_skyrimPath.Text}";
            return;
        }

        _skyrimPath.Text = string.Empty;
        _status.Text = "First run: select this PC's Skyrim Special Edition + SKSE install folder.";
        BrowseSkyrim();
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
        if (_launching) return;
        _launching = true;
        try
        {
            if (!IsValidSkyrimFolder(_skyrimPath.Text, out _))
            {
                if (!BrowseSkyrim()) return;
            }

            var config = CurrentConfig();
            ValidateSkyrim(config.SkyrimPath);
            SaveConfig(config);
            InstallBundledClientIfPresent(config.SkyrimPath);

            var pluginDir = Path.Combine(config.SkyrimPath, "Data", "SKSE", "Plugins");
            Directory.CreateDirectory(pluginDir);
            var enabled = Path.Combine(pluginDir, "SkyrimMultiplayer.dll");
            var disabled = Path.Combine(pluginDir, "SkyrimMultiplayer.dll.disabled");

            if (multiplayer)
            {
                if (!File.Exists(enabled) && File.Exists(disabled)) File.Move(disabled, enabled, true);
                if (!File.Exists(enabled)) throw new InvalidOperationException("SkyrimMultiplayer.dll is not installed.");

                var ip = await ResolveServerAddressAsync(config.ServerAddress, _shutdown.Token);
                File.WriteAllText(
                    Path.Combine(pluginDir, "SkyrimMPClient.ini"),
                    $"[Server]{Environment.NewLine}Address={ip}{Environment.NewLine}Port={config.ServerPort}{Environment.NewLine}");
                _status.Text = $"{BuildInfo.Display}. Direct server: {ip}:{config.ServerPort}. Load SkyrimMP_* (or a post-Helgen save for first import).";
            }
            else
            {
                if (File.Exists(enabled)) File.Move(enabled, disabled, true);
                _status.Text = "Single-player mode: multiplayer plugin disabled for this launch.";
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
            _status.Text = multiplayer ? "Multiplayer client closed. Close this launcher when finished." : "Single-player Skyrim closed.";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, ex.Message, "SkyrimMP Launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
            _status.Text = "Launch failed.";
        }
        finally
        {
            _launching = false;
        }
    }

    private static async Task<IPAddress> ResolveServerAddressAsync(string address, CancellationToken token)
    {
        if (string.IsNullOrWhiteSpace(address))
            throw new InvalidOperationException("Enter the IPv4 address or hostname of the SkyrimMP server.");

        if (IPAddress.TryParse(address, out var parsed))
        {
            if (parsed.AddressFamily == AddressFamily.InterNetwork) return parsed;
            throw new InvalidOperationException("The SkyrimMP client currently requires an IPv4 server address.");
        }

        try
        {
            var addresses = await Dns.GetHostAddressesAsync(address, AddressFamily.InterNetwork, token);
            var resolved = addresses.FirstOrDefault(candidate => candidate.AddressFamily == AddressFamily.InterNetwork);
            return resolved ?? throw new InvalidOperationException($"Hostname '{address}' has no IPv4 address.");
        }
        catch (SocketException ex)
        {
            throw new InvalidOperationException($"Could not resolve SkyrimMP server hostname '{address}'.", ex);
        }
    }

    private static bool IsValidSkyrimFolder(string path, out string problem)
    {
        if (string.IsNullOrWhiteSpace(path) || !Directory.Exists(path))
        {
            problem = "The selected folder does not exist.";
            return false;
        }

        if (!File.Exists(Path.Combine(path, "SkyrimSE.exe")))
        {
            problem = "SkyrimSE.exe was not found in that folder.";
            return false;
        }

        if (!File.Exists(Path.Combine(path, "skse64_loader.exe")))
        {
            problem = "skse64_loader.exe was not found in that folder. Install the matching SKSE64 build first.";
            return false;
        }

        problem = string.Empty;
        return true;
    }

    private static void ValidateSkyrim(string path)
    {
        if (!IsValidSkyrimFolder(path, out var problem))
            throw new InvalidOperationException(problem);
    }

    private static void InstallBundledClientIfPresent(string skyrimPath)
    {
        var bundled = Path.Combine(AppContext.BaseDirectory, "SkyrimMultiplayer.dll");
        if (!File.Exists(bundled)) return;
        var pluginDir = Path.Combine(skyrimPath, "Data", "SKSE", "Plugins");
        Directory.CreateDirectory(pluginDir);
        File.Copy(bundled, Path.Combine(pluginDir, "SkyrimMultiplayer.dll"), true);
    }

}
