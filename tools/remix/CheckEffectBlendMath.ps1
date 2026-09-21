$ErrorActionPreference = 'Stop'
# CPU reference checks for native standard-alpha WBOIT. These establish the
# compositing identities, not GPU shader execution or an in-game visual pass.
function Resolve-Layers($Layers, [double]$Compensation) {
    [double]$legacySum = 0
    [double]$nativeSum = 0
    [double]$weightSum = 0
    [double]$transmittance = 1
    foreach ($layer in $Layers) {
        if ($layer.Native) { $nativeSum += $layer.Premultiplied * $layer.Weight }
        else { $legacySum += $layer.Premultiplied * $layer.Alpha * $layer.Weight }
        $weightSum += $layer.Alpha * $layer.Weight
        $transmittance *= 1 - $layer.Alpha
    }
    $radiance = if ($weightSum -gt 0) {
        ($legacySum * $Compensation + $nativeSum) / $weightSum * (1 - $transmittance)
    } else { 0 }
    return @{ Radiance = $radiance; Transmittance = $transmittance }
}
function Assert-Near([double]$Actual, [double]$Expected, [string]$Case) {
    if (![double]::IsFinite($Actual) -or [Math]::Abs($Actual - $Expected) -gt 1e-10) {
        throw "$Case : expected $Expected, got $Actual"
    }
}
$cases = 0
foreach ($alpha in @(0.0, 0.001, 0.1, 0.25, 0.5, 0.99, 1.0)) {
    foreach ($weight in @(0.01, 1.0, 10.0)) {
        foreach ($compensation in @(0.0, 1.0, 4.0)) {
            $layer = @{ Native = $true; Alpha = $alpha; Premultiplied = 0.7 * $alpha; Weight = $weight }
            $result = Resolve-Layers @($layer) $compensation
            Assert-Near $result.Radiance (0.7 * $alpha) 'single native layer'
            Assert-Near $result.Transmittance (1 - $alpha) 'single native transmittance'
            $layer.Native = $false
            $result = Resolve-Layers @($layer) $compensation
            Assert-Near $result.Radiance (0.7 * $alpha * $alpha * $compensation) 'legacy behavior preserved'
            $cases += 3
        }
    }
}
# Equal-color layers have the exact source-over result irrespective of weights.
$layers = @(
    @{ Native = $true; Alpha = 0.2; Premultiplied = 0.7 * 0.2; Weight = 0.4 },
    @{ Native = $true; Alpha = 0.6; Premultiplied = 0.7 * 0.6; Weight = 2.0 }
)
foreach ($compensation in @(0.0, 1.0, 4.0)) {
    $result = Resolve-Layers $layers $compensation
    Assert-Near $result.Radiance (0.7 * (1 - 0.8 * 0.4)) 'two native layers'
    Assert-Near $result.Transmittance (0.8 * 0.4) 'two native transmittance'
    $cases += 2
}
Write-Output "$cases native-alpha/legacy compositing reference assertions passed (not a GPU test)."
