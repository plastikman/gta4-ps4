# Host-side policy checks only; not a substitute for an Orbis build/console run.
$ErrorActionPreference = 'Stop'
$cases = 0
foreach ($interval in 0..4) {
    foreach ($frame in 0..12) {
        foreach ($device in @($false, $true)) {
            foreach ($minimum in @(0, 1000)) {
                foreach ($count in @(0, 999, 1000, 2000)) {
                    $rest = $interval -eq 0 -or ($interval -gt 1 -and $device -and
                        ($frame + 1) % $interval -ne 0)
                    $actual = $device -and ($rest -or ($minimum -ne 0 -and $count -lt $minimum))
                    if ($interval -eq 0) {
                        if ($actual -ne $device) { throw 'Shadow-off policy mismatch' }
                    } else {
                        $oldRest = $interval -gt 1 -and $device -and ($frame + 1) % $interval -ne 0
                        $old = $oldRest -or ($minimum -ne 0 -and $count -lt $minimum -and $device)
                        if ($actual -ne $old) { throw 'Enabled-shadow policy regression' }
                    }
                    $cases++
                }
            }
        }
    }
}

$root = Split-Path $PSScriptRoot -Parent
$hooks = Get-Content -Raw (Join-Path $root 'glue/rexglue-sdk-main/gta4-recomp/src/gta4_native_hooks.cpp')
if ($hooks -notmatch 'const bool shadow_rest = interval == 0 \|\|' -or
    $hooks -notmatch 'if \(!device \|\| \(!shadow_rest') {
    throw 'Source no longer matches the shadow policy modeled by this test; review the test'
}
$renderer = Get-Content -Raw (Join-Path $root 'glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp')
if (-not $renderer.Contains('!((REX_PLATFORM_PS5 || REX_PLATFORM_PS4) && REXCVAR_GET(ps5_hud_trace))')) {
    throw 'PS4/PS5 HUD diagnostic gate missing'
}
Write-Output "PASS: $cases shadow policy cases and source guards (not compiled C++ tests)."
