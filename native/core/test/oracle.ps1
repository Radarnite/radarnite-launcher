# Oracle: which BMP code points does 2.2.1's config writer (UnsafeRelaxedJsonEscaping) escape, and which code page does
# its stdout use for non-ASCII text. Works on the scratch ref tree only; config.json restored at the end.
param([string]$Dir = 'S:\rl-core-test')
$ErrorActionPreference = 'Stop'
$t = $Dir
$ref = Join-Path $t 'rlcoretest-ref'
$old = Join-Path $ref 'RadarniteLauncher.exe'
$cfgPath = Join-Path $ref 'config.json'
$backup = [IO.File]::ReadAllBytes($cfgPath)
$utf8 = New-Object Text.UTF8Encoding $false
function Run([string]$exe, [string]$argline) {
  $psi = New-Object Diagnostics.ProcessStartInfo $exe, $argline
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
  $p = [Diagnostics.Process]::Start($psi); $ms = New-Object IO.MemoryStream
  $e = $p.StandardError.ReadToEndAsync(); $p.StandardOutput.BaseStream.CopyTo($ms); $p.WaitForExit()
  [pscustomobject]@{ bytes = $ms.ToArray(); code = $p.ExitCode }
}
try {
  $sb = New-Object Text.StringBuilder
  for ($c = 0; $c -lt 0x10000; $c++) { if ($c -ge 0xD800 -and $c -le 0xDFFF) { continue }; [void]$sb.AppendFormat('\u{0:x4}', $c) }
  $json = '{"displayName":"A","accountId":"0123456789abcdef0123456789abcdef","practicePlaylist":"' + $sb.ToString() + '","buildsFolder":"S:\\FortniteBuilds"}'
  [IO.File]::WriteAllText($cfgPath, $json, $utf8)
  $r = Run $old '--check zz-no-such-build'
  Copy-Item $cfgPath (Join-Path $t 'out\oracle-config.json') -Force
  "oracle run exit $($r.code), config $((Get-Item $cfgPath).Length) bytes"

  # stdout encoding probe
  $json = '{"displayName":"\u00d1\u20ac\u00ff\u4e2d\u00e9\u0101","accountId":"0123456789abcdef0123456789abcdef","buildsFolder":"S:\\FortniteBuilds"}'
  [IO.File]::WriteAllText($cfgPath, $json, $utf8)
  $r = Run $old '--check zz-no-such-build'
  "stdout bytes: " + (($r.bytes | ForEach-Object { '{0:x2}' -f $_ }) -join ' ')
  $cp = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage'
  "ACP $($cp.ACP) OEMCP $($cp.OEMCP); this PowerShell console output encoding: $([Console]::OutputEncoding.CodePage)"
} finally {
  [IO.File]::WriteAllBytes($cfgPath, $backup)
  "config restored"
}
