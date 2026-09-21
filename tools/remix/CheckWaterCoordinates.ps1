# Algebraic regression for Water.hlsl's non-UNIFIED_WATER vertex coordinates.
# This is independent of a running game; it does not assert rendered parity.
$ErrorActionPreference = 'Stop'
$cases = 0
$maximumError = 0.0
foreach ($dimension in @(3.0, 5.0, 7.0, 9.0)) {
    foreach ($cell in @(@(0.03125, 0.15625, 2.0, -4.0), @(-0.875, 0.375, -6.0, 8.0))) {
        foreach ($origin in @(@(0.125, 0.875), @(0.999, 0.001))) {
            foreach ($u in @(-0.25, 0.0, 0.13, 0.5, 0.97, 1.0, 1.25)) {
                foreach ($v in @(-0.25, 0.0, 0.13, 0.5, 0.97, 1.0, 1.25)) {
                    # Direct native VS equations (already computed ObjectUV.yz).
                    $expected = @(
                        (($u - 0.5) * 0.1 + $cell[0] + $cell[2] / $dimension),
                        (($v - 0.5) * 0.1 + $cell[1] + (-$cell[3] + $dimension) / $dimension),
                        (-0.25 + $u * 0.5 + $origin[0]),
                        (-0.25 + $v * 0.5 + $origin[1])
                    )
                    # CS material packing followed by the Remix shader equations.
                    $packed = @(
                        (($cell[0] - 0.05) * $dimension + $cell[2]),
                        (($cell[1] - 0.05) * $dimension - $cell[3] + $dimension),
                        ($origin[0] - 0.25), ($origin[1] - 0.25)
                    )
                    $actual = @(
                        (($packed[0] + $u * 0.1 * $dimension) / $dimension),
                        (($packed[1] + $v * 0.1 * $dimension) / $dimension),
                        ($packed[2] + $u * 0.5), ($packed[3] + $v * 0.5)
                    )
                    for ($axis = 0; $axis -lt 4; ++$axis) {
                        $error = [Math]::Abs($expected[$axis] - $actual[$axis])
                        $maximumError = [Math]::Max($maximumError, $error)
                        if ($error -gt 1e-12) { throw "Wading coordinate mismatch: dimension=$dimension, uv=$u,$v, axis=$axis, error=$error" }
                    }
                    ++$cases
                }
            }
        }
    }
}
[ordered]@{ cases = $cases; maximumError = $maximumError; note = 'Algebra only, before GPU half-float packing; not a rendered-image test.' } | ConvertTo-Json
