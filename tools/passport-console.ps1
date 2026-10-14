<#
.SYNOPSIS
    Drive the Passport's serial console: run commands, wait for the prompt, print what came back.

.DESCRIPTION
    The device's serial console is the whole diagnostic surface, and driving it by hand is how every
    verification in docs/assets/deepseek-passport/progress-log.md was done. This exists so that the same
    thing can be done in one command, and so that tests do not have to guess at timing:

      * it waits for the console prompt instead of sleeping for a fixed time, which is the difference
        between a reliable reading and an intermittent one;
      * it can wait out the boot before sending anything (opening the port resets this board);
      * it can keep the log lines, drop them, or show only the ones that matter.

    No dependencies: pyserial is not installed for the system interpreter, and the platform's own serial
    API is available everywhere this repository is used with a board attached.

.EXAMPLE
    ./tools/passport-console.ps1 -Command "prov status"
    ./tools/passport-console.ps1 -Command "prov events" -Baud 115200
    ./tools/passport-console.ps1 -Script tools/checks/hotspot.txt
    ./tools/passport-console.ps1 -Interactive
#>
[CmdletBinding(DefaultParameterSetName = 'Command')]
param(
    [string]$Port = "COM47",
    [int]$Baud = 115200,
    [Parameter(ParameterSetName = 'Command', Mandatory = $true)]
    [string]$Command,
    [Parameter(ParameterSetName = 'Script', Mandatory = $true)]
    [string]$Script,
    [Parameter(ParameterSetName = 'Interactive', Mandatory = $true)]
    [switch]$Interactive,
    # Wait this long for the console prompt after opening the port (the board reboots when it opens).
    [int]$BootWaitMs = 15000,
    # Per-command time budget once the prompt has appeared.
    [int]$TimeoutMs = 30000,
    [switch]$KeepLogs,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

function Open-Console {
    $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
    $sp.ReadTimeout = 200
    $sp.Encoding = [System.Text.Encoding]::UTF8
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.Open()
    return $sp
}

function Read-Until {
    param($sp, [string]$Pattern, [int]$TimeoutMs)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    # Bytes, decoded as UTF-8 at the end: the device speaks Chinese in its logs and its interface, and
    # appending one char per byte turns every one of those into mojibake.
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ((Get-Date) -lt $deadline) {
        $got = $false
        try {
            while ($sp.BytesToRead -gt 0) {
                $x = $sp.ReadByte()
                if ($x -ge 0) { $bytes.Add([byte]$x); $got = $true }
            }
        } catch { break }
        $text = [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
        if ($text -match $Pattern) { break }
        if (-not $got) { Start-Sleep -Milliseconds 25 }
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
}

function Show-Output {
    param([string]$text, [switch]$KeepLogs)
    $lines = $text -split "`r?`n"
    foreach ($line in $lines) {
        if ($line.Trim() -eq '') { continue }
        # Log lines carry a level and a tag: "I (1234) tag: message". They are noise unless asked for.
        if (-not $KeepLogs -and $line -match '^\s*[IWE]\s\(\d+\)') { continue }
        Write-Output $line.TrimEnd()
    }
}

$sp = $null
try {
    $sp = Open-Console
    # Opening the port resets this board, so wait for the prompt before typing: the boot log is not an
    # answer to anything.
    $boot = Read-Until -sp $sp -Pattern 'passport>' -TimeoutMs $BootWaitMs
    if ($boot -notmatch 'passport>') {
        Write-Warning "no console prompt within ${BootWaitMs}ms; sending anyway"
    }

    $commands = @()
    if ($PSCmdlet.ParameterSetName -eq 'Command') {
        $commands = @($Command)
    } elseif ($PSCmdlet.ParameterSetName -eq 'Script') {
        $commands = Get-Content -LiteralPath $Script | Where-Object { $_ -and -not $_.StartsWith('#') }
    }

    if ($PSCmdlet.ParameterSetName -eq 'Interactive') {
        Write-Output "passport console on $Port. Type a command, or 'exit'."
        while ($true) {
            $line = Read-Host 'passport>'
            if ($line -eq 'exit' -or $line -eq 'quit') { break }
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            $sp.DiscardInBuffer()
            $sp.Write($line + "`r`n")
            $out = Read-Until -sp $sp -Pattern 'passport>' -TimeoutMs $TimeoutMs
            Show-Output -text $out -KeepLogs:$KeepLogs
        }
        return
    }

    foreach ($c in $commands) {
        # A script line may be "sleep <ms>": some behaviour only happens with time passing, such as a
        # confirmation that expires if nobody answers, and a test that cannot wait cannot check it.
        if ($c -match '^sleep\s+(\d+)$') {
            Start-Sleep -Milliseconds ([int]$Matches[1])
            if (-not $Quiet) { Write-Output "--- waited $($Matches[1]) ms" }
            continue
        }
        if (-not $Quiet) { Write-Output "--- $c" }
        $sp.DiscardInBuffer()
        $sp.Write($c + "`r`n")
        $out = Read-Until -sp $sp -Pattern 'passport>' -TimeoutMs $TimeoutMs
        Show-Output -text $out -KeepLogs:$KeepLogs
    }
} finally {
    if ($sp -ne $null) {
        if ($sp.IsOpen) { $sp.Close() }
        $sp.Dispose()
    }
}
