#requires -Version 7.0
param(
    [Parameter(Mandatory)] [string]$Cli,
    [Parameter(Mandatory)] [string]$EmbeddingModel,
    [Parameter(Mandatory)] [string]$GenerationModel
)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Cli).Path
$demo = Join-Path $PSScriptRoot '../data/demo'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.ComponentModel;
public static class RagEndpointObserver {
    [DllImport("iphlpapi.dll")]
    static extern uint GetExtendedTcpTable(IntPtr table, ref int size, bool order, int family, int tableClass, int reserved);
    [DllImport("iphlpapi.dll")]
    static extern uint GetExtendedUdpTable(IntPtr table, ref int size, bool order, int family, int tableClass, int reserved);
    static int Count(int pid, int family, bool tcp, int stride) {
        int size = 65536;
        while (true) {
            IntPtr table = Marshal.AllocHGlobal(size);
            try {
                uint rc = tcp ? GetExtendedTcpTable(table, ref size, false, family, 5, 0)
                              : GetExtendedUdpTable(table, ref size, false, family, 1, 0);
                if (rc == 122) continue;
                if (rc != 0) throw new Win32Exception((int)rc);
                int rows = Marshal.ReadInt32(table);
                int hits = 0;
                for(int i=0; i<rows; ++i)
                    if (Marshal.ReadInt32(table, 4 + i*stride + stride - 4) == pid) ++hits;
                return hits;
            } finally { Marshal.FreeHGlobal(table); }
        }
    }
    public static int Count(int pid) {
        return Count(pid, 2, true, 24) + Count(pid, 23, true, 56) +
               Count(pid, 2, false, 12) + Count(pid, 23, false, 28);
    }
}
'@
$baseline = [RagEndpointObserver]::Count($PID)
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
try {
    $listener.Start()
    if ([RagEndpointObserver]::Count($PID) -le $baseline) { throw 'Observer missed its positive control' }
    Write-Output 'TCP/IPv4 loopback listener detected in positive control'
} finally { $listener.Stop() }
function Observe-Rag([string]$label, [string[]]$cliArgs) {
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $null = $info.Environment.Remove('OPENAI_API_KEY')
    foreach ($arg in $cliArgs) { $info.ArgumentList.Add($arg) }
    $proc = [Diagnostics.Process]::Start($info)
    $samples = 0
    $rows = 0
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $maxGap = 0
    $last = 0
    do {
        $rows += [RagEndpointObserver]::Count($proc.Id)
        $samples++
        $now = $timer.ElapsedMilliseconds
        $maxGap = [Math]::Max($maxGap, $now - $last)
        $last = $now
    } while (-not $proc.WaitForExit(3))
    Write-Output "$label pid=$($proc.Id) samples=$samples socket_rows=$rows max_interval_ms=$maxGap elapsed_ms=$($timer.ElapsedMilliseconds) exit=$($proc.ExitCode)"
    $proc.StandardOutput.ReadToEnd()
    if ($proc.ExitCode) { throw $proc.StandardError.ReadToEnd() }
    if ($rows) { throw "Observed $rows TCP/UDP rows for $label" }
    $proc.Dispose()
}
$db = Join-Path ([IO.Path]::GetTempPath()) ("rag-observe-" + [guid]::NewGuid() + ".sqlite")
$localArgs = @('--db',$db,'--embedding-model',$EmbeddingModel,'--local-model',$GenerationModel)
try {
    Observe-Rag 'import + exact question' ($localArgs + @("$demo/contract.txt","$demo/decisions.txt",'Резервное копирование'))
    Observe-Rag 'question' ($localArgs + 'В какой день делают страховочную копию данных проекта?')
    Observe-Rag 'history' @('--history',$db)
} finally {
    if (Test-Path -LiteralPath $db) { Remove-Item -LiteralPath $db }
}
