param(
    [Parameter(Mandatory = $false)]
    [string]$ServerAddress,

    [Parameter(Mandatory = $false)]
    [int]$ServerPort = 10578
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ServerAddress)) {
    $ServerAddress = Read-Host 'Enter the SkyrimMP server IPv4 address'
}

if ([string]::IsNullOrWhiteSpace($ServerAddress)) {
    throw 'A server IPv4 address is required.'
}

$parsedAddress = $null
if (-not [System.Net.IPAddress]::TryParse($ServerAddress, [ref]$parsedAddress)) {
    throw "'$ServerAddress' is not a valid IPv4 address."
}
if ($parsedAddress.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'This test relay currently supports IPv4 only.'
}
if ($ServerPort -lt 1 -or $ServerPort -gt 65535) {
    throw 'ServerPort must be between 1 and 65535.'
}

$listenAddress = [System.Net.IPAddress]::Loopback
$listenEndpoint = New-Object System.Net.IPEndPoint($listenAddress, 10578)
$serverEndpoint = New-Object System.Net.IPEndPoint($parsedAddress, $ServerPort)

$relay = New-Object System.Net.Sockets.UdpClient
$relay.Client.Bind($listenEndpoint)
$relay.Client.ReceiveTimeout = 50

$upstream = New-Object System.Net.Sockets.UdpClient
$upstream.Client.ReceiveTimeout = 50

$clientEndpoint = $null

Write-Host ''
Write-Host 'SkyrimMP test relay running.'
Write-Host "  Local client target : 127.0.0.1:10578"
Write-Host "  Remote server       : $ServerAddress`:$ServerPort"
Write-Host ''
Write-Host 'Leave this window open, then launch Skyrim through SKSE.'
Write-Host 'Press Ctrl+C here after Skyrim is closed.'
Write-Host ''

try {
    while ($true) {
        try {
            $fromClient = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $payload = $relay.Receive([ref]$fromClient)
            if ($payload -and $payload.Length -gt 0) {
                $clientEndpoint = $fromClient
                [void]$upstream.Send($payload, $payload.Length, $serverEndpoint)
            }
        }
        catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut -and
                $_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::WouldBlock) {
                throw
            }
        }

        try {
            $fromServer = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $payload = $upstream.Receive([ref]$fromServer)
            if ($payload -and $payload.Length -gt 0 -and $null -ne $clientEndpoint) {
                [void]$relay.Send($payload, $payload.Length, $clientEndpoint)
            }
        }
        catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut -and
                $_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::WouldBlock) {
                throw
            }
        }
    }
}
finally {
    $relay.Close()
    $upstream.Close()
}
