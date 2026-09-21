"""Exploratory native/Remix raw-buffer comparison, not a fidelity pass/fail test."""

import argparse
import json
from pathlib import Path

import numpy as np

from ReadBuffers import read_dds
from NormalBuffers import angle_degrees, native_world_normals, remix_world_normals


def validate_capture_pair(native, remix):
    """A paired capture must have two successful halves of the same host frame."""
    if not native.get("pairedCapture") and not remix.get("pairedCapture"):
        return False
    if not (native.get("pairedCapture") and remix.get("pairedCapture") and
            native.get("nativeReference") and not remix.get("nativeReference") and
            native.get("nativePreparation") and remix.get("nativePreparation") and
            remix.get("queued") and remix.get("pairedNativeCaptured")):
        raise ValueError("Incomplete paired capture")
    if any(native.get(key) is None or native[key] != remix.get(key) for key in ("pid", "request", "frame")):
        raise ValueError("Paired capture frame/identity mismatch")
    return True


def normal_agreement_diagnostics(native_normals, remix_normals, normal_angles,
                                 eligible, absolute_depth_error):
    """Sensitivity probes only: rejecting difficult pixels cannot certify parity."""
    gates = []
    for threshold in (10.0, 2.0, 0.5, 0.1):
        selected = eligible & np.isfinite(absolute_depth_error) & (absolute_depth_error < threshold)
        angles = normal_angles[selected]
        gates.append({"absoluteDepthErrorLimit": threshold, "samples": int(selected.sum()),
                      "anglePercentiles": np.percentile(angles, [50, 90, 99]).tolist() if len(angles) else None,
                      "over30DegreeFraction": float(np.mean(angles > 30)) if len(angles) else None})
    # Interior material masks can still span adjacent/overlapping blades.
    # Restricting normal discontinuities is diagnostic, not a parity gate.
    height, width = eligible.shape
    continuous = np.ones((height, width), dtype=bool)
    for normals in (native_normals, remix_normals):
        padded_normals = np.pad(normals, ((1, 1), (1, 1), (0, 0)), mode="edge")
        for dy in range(3):
            for dx in range(3):
                neighbour = padded_normals[dy:dy + height, dx:dx + width]
                continuous &= np.sum(normals * neighbour, axis=-1) > np.cos(np.deg2rad(10))
    selected = eligible & continuous
    return {
        "absoluteDepthGateDiagnostics": gates,
        "continuousNormalDiagnostic": {
            "scope": "Both images' 3x3 normal neighbourhood within 10deg; excludes internal blade boundaries, NOT a success criterion",
            "samples": int(selected.sum()),
            "anglePercentiles": np.percentile(normal_angles[selected], [50, 90, 99]).tolist() if selected.any() else None,
        },
    }


def compare(native_directory, remix_directory, baseline_remix_directory=None):
    native_metadata = json.loads(next(native_directory.glob("native-*.json")).read_text())
    remix_metadata = json.loads(next(remix_directory.glob("rtx-*.json")).read_text())
    paired = validate_capture_pair(native_metadata, remix_metadata)
    native_manifest = json.loads((native_directory / "manifest.json").read_text(encoding="utf-8-sig"))
    remix_manifest = json.loads((remix_directory / "manifest.json").read_text(encoding="utf-8-sig"))
    cameras = [m[key] for m in (native_manifest, remix_manifest) for key in ("cameraBefore", "cameraAfter")]
    if any(camera != cameras[0] for camera in cameras[1:]):
        raise ValueError("Camera records differ; do not compare this pair as matching views")
    native, _ = read_dds(next(native_directory.glob("*-albedo.dds")))
    depth, _ = read_dds(next(native_directory.glob("*-depth.dds")))
    remix, _ = read_dds(next(remix_directory.glob("gbufferAlbedo_*.dds")), remix_vulkan_packing=True)
    remix_z, _ = read_dds(next(remix_directory.glob("gbufferLinearZ_*.dds")))
    native_normal_pixels, _ = read_dds(next(native_directory.glob("*-normalRoughness.dds")))
    normal_path = next(remix_directory.glob("gbufferWorldNormals_*.dds"), None)
    remix_normal_pixels, _ = read_dds(normal_path or next(remix_directory.glob("worldNormals_*.dds")))
    native_normals = native_world_normals(native_normal_pixels, native_metadata["shadowView"])
    remix_normals = remix_world_normals(remix_normal_pixels)
    height, width = remix.shape[:2]
    # Pixel-centre nearest resampling preserves raw values. It does not account
    # for differing TAA jitter, ray footprints, wind, alpha tests or water PSR.
    yy = np.minimum(((np.arange(height) + 0.5) * native.shape[0] / height).astype(int), native.shape[0] - 1)
    xx = np.minimum(((np.arange(width) + 0.5) * native.shape[1] / width).astype(int), native.shape[1] - 1)
    native = native[yy[:, None], xx[None, :], :3]
    native_normals = native_normals[yy[:, None], xx[None, :]]
    normal_angles = angle_degrees(native_normals, remix_normals)
    depth = depth[yy[:, None], xx[None, :], 0]
    projection = native_metadata["shadowProjection"]
    native_z = projection[14] / (depth - projection[10])
    relative_z = np.abs(remix_z[..., 0] - native_z) / np.maximum(np.abs(native_z), 1)
    baseline_angles = baseline_relative_z = None
    if baseline_remix_directory is not None:
        baseline_manifest = json.loads((baseline_remix_directory / "manifest.json").read_text(encoding="utf-8-sig"))
        if any(baseline_manifest[key] != cameras[0] for key in ("cameraBefore", "cameraAfter")):
            raise ValueError("Baseline camera differs from the comparison view")
        baseline_normal_path = next(baseline_remix_directory.glob("gbufferWorldNormals_*.dds"), None)
        baseline_pixels, _ = read_dds(baseline_normal_path or next(baseline_remix_directory.glob("worldNormals_*.dds")))
        baseline_z, _ = read_dds(next(baseline_remix_directory.glob("gbufferLinearZ_*.dds")))
        if baseline_pixels.shape[:2] != (height, width) or baseline_z.shape[:2] != (height, width):
            raise ValueError("Baseline Remix buffer dimensions differ")
        baseline_angles = angle_degrees(native_normals, remix_world_normals(baseline_pixels))
        baseline_relative_z = np.abs(baseline_z[..., 0] - native_z) / np.maximum(np.abs(native_z), 1)
    # Regions manually located in the 1280x720 grass-bank capture. They are
    # mixed-pixel probes, not material-ID masks or universal validation ROIs.
    if (width, height) != (1280, 720):
        raise ValueError("Grass-bank probes require the original 1280x720 capture")
    regions = {"rock": (1100, 70, 1220, 140), "ground": (145, 195, 225, 240),
               "grass_band_mixed": (165, 135, 260, 173), "stump": (425, 218, 470, 233)}
    result = {"camera": cameras[0], "scope": "Exploratory mixed-region statistics, not parity certification",
              "sampling": "Native nearest resampled; depth filter <1%; no jitter/wind/PSR registration",
              "nativeProjectionDepthFormula": "projection[14] / (depth - projection[10])", "regions": {}}
    result["sameHostFramePair"] = paired
    result["remixNormalBuffer"] = normal_path.name if normal_path else "legacy post-composite worldNormals"
    if paired:
        result["sampling"] = "Same host frame; native nearest resampled; depth filter <1%; no jitter/footprint/PSR registration"
    if remix_manifest.get("debugView") == 804:
        classification, _ = read_dds(next(remix_directory.glob("rtxImageDebugView_*.dds")))
        cy = np.minimum(((np.arange(height) + 0.5) * classification.shape[0] / height).astype(int), classification.shape[0] - 1)
        cx = np.minimum(((np.arange(width) + 0.5) * classification.shape[1] / width).astype(int), classification.shape[1] - 1)
        classification = classification[cy[:, None], cx[None, :], :3]
        masks = {
            "ordinary": (classification[..., 1] > 0.9) & (classification[..., 0] < 0.1) & (classification[..., 2] < 0.1),
            "complex": (classification[..., 2] > 0.9) & (classification[..., 0] < 0.1) & (classification[..., 1] < 0.1),
        }
        result["grassClassification"] = {
            "scope": "Same-frame RTX debug804, pixel-centre resampled, excludes mixed-edge colours; no native material-ID mask",
            "kinds": {},
        }
        for kind, mask in masks.items():
            matched = mask & np.isfinite(relative_z) & (relative_z < 0.01)
            padded = np.pad(mask, 1, constant_values=False)
            interior = np.logical_and.reduce([padded[dy:dy + height, dx:dx + width] for dy in range(3) for dx in range(3)])
            interior_matched = interior & np.isfinite(relative_z) & (relative_z < 0.01)
            # Ordinary CS Grass Lighting writes specColor.w=1 into this channel.
            # Necessary for ordinary grass, but not a unique native material ID.
            native_gloss = native_normal_pixels[yy[:, None], xx[None, :], 2]
            gloss_one = matched & (native_gloss >= 0.999)
            result["grassClassification"]["kinds"][kind] = {
                "pixels": int(mask.sum()), "depthMatchedPixels": int(matched.sum()),
                "normalAngleDegreesPercentiles": np.percentile(normal_angles[matched], [10, 50, 90, 99]).tolist() if matched.any() else None,
                "interior3x3DepthMatchedPixels": int(interior_matched.sum()),
                "interior3x3NormalAngleDegreesPercentiles": np.percentile(normal_angles[interior_matched], [10, 50, 90, 99]).tolist() if interior_matched.any() else None,
                "nativeGlossOnePixels": int(gloss_one.sum()),
                "nativeGlossOneNormalAngleDegreesPercentiles": np.percentile(normal_angles[gloss_one], [10, 50, 90, 99]).tolist() if gloss_one.any() else None,
            }
            tiles = []
            for tile_y in range(0, height, 32):
                for tile_x in range(0, width, 32):
                    sl = np.s_[tile_y:tile_y + 32, tile_x:tile_x + 32]
                    selected = interior_matched[sl] & (native_gloss[sl] >= 0.999)
                    if selected.sum() < 32:
                        continue
                    angles = normal_angles[sl][selected]
                    bad = selected & (normal_angles[sl] > 30)
                    if not bad.any():
                        continue
                    tiles.append({
                        "bounds": [tile_x, tile_y, tile_x + 32, tile_y + 32],
                        "samples": int(selected.sum()), "over30DegreeSamples": int(bad.sum()),
                        "medianAngleDegrees": float(np.median(angles)),
                        "badNativeNormalMean": native_normals[sl][bad].mean(axis=0).tolist(),
                        "badRemixNormalMean": remix_normals[sl][bad].mean(axis=0).tolist(),
                        "badNativeMedianRGB": np.median(native[sl][bad], axis=0).tolist(),
                        "badRemixMedianRGB": np.median(remix[sl][bad, :3], axis=0).tolist(),
                        "badMedianRelativeDepthError": float(np.median(relative_z[sl][bad])),
                    })
            result["grassClassification"]["kinds"][kind]["outlierTiles"] = sorted(
                tiles, key=lambda tile: tile["over30DegreeSamples"], reverse=True)[:8]
            result["grassClassification"]["kinds"][kind].update(normal_agreement_diagnostics(
                native_normals, remix_normals, normal_angles,
                interior_matched & (native_gloss >= 0.999), np.abs(remix_z[..., 0] - native_z)))
    for name, (x0, y0, x1, y1) in regions.items():
        error_z = relative_z[y0:y1, x0:x1]
        valid = np.isfinite(error_z) & (error_z < 0.01)
        n = native[y0:y1, x0:x1][valid]
        r = remix[y0:y1, x0:x1, :3][valid]
        if not len(n):
            raise ValueError(f"No approximately depth-matched samples: {name}")
        # Both are explicit hypotheses: raw native buffer and power-2.2 decode.
        # Neither is declared the correct physical colour domain by this tool.
        result["regions"][name] = {
            "bounds": [x0, y0, x1, y1], "samples": len(n), "depthMatchFraction": float(valid.mean()),
            "relativeDepthErrorPercentiles": np.percentile(error_z, [50, 90, 99]).tolist(),
            "nativeMedianRGB": np.median(n, axis=0).tolist(), "remixMedianRGB": np.median(r, axis=0).tolist(),
            "rawNativeMAE": np.mean(np.abs(r - n), axis=0).tolist(),
            "pow22NativeMAE": np.mean(np.abs(r - n ** 2.2), axis=0).tolist(),
            "normalAngleDegreesPercentiles": np.percentile(normal_angles[y0:y1, x0:x1][valid], [10, 50, 90, 99]).tolist(),
            "nativeNormalMeanXYZ": np.mean(native_normals[y0:y1, x0:x1][valid], axis=0).tolist(),
            "remixNormalMeanXYZ": np.mean(remix_normals[y0:y1, x0:x1][valid], axis=0).tolist(),
        }
        if baseline_angles is not None:
            baseline_error = baseline_relative_z[y0:y1, x0:x1]
            common = valid & np.isfinite(baseline_error) & (baseline_error < 0.01)
            if not common.any():
                raise ValueError(f"No common depth-matched normal samples: {name}")
            before = baseline_angles[y0:y1, x0:x1][common]
            after = normal_angles[y0:y1, x0:x1][common]
            result["regions"][name]["pairedNormals"] = {
                "baseline": str(baseline_remix_directory),
                "scope": "Same pixel locations and shared depth mask; NOT material-ID or temporal registration",
                "samples": int(common.sum()),
                "beforeAnglePercentiles": np.percentile(before, [10, 50, 90, 99]).tolist(),
                "afterAnglePercentiles": np.percentile(after, [10, 50, 90, 99]).tolist(),
                "angleChangePercentiles": np.percentile(after - before, [10, 50, 90, 99]).tolist(),
                "meanAngleChange": float(np.mean(after - before)),
                "fractionImproved": float(np.mean(after < before)),
            }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native_directory", type=Path)
    parser.add_argument("remix_directory", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--baseline-remix-directory", type=Path)
    args = parser.parse_args()
    report = json.dumps(compare(args.native_directory, args.remix_directory, args.baseline_remix_directory), indent=2)
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
    print(report)
