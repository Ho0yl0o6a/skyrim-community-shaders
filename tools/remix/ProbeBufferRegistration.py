"""Depth-only global-shift diagnostic; never a renderer parity certificate.

Fit a single translation on the bank rock's depth, then evaluate held-out grass
normals without fitting them. No per-pixel correspondence search is allowed.
"""

import argparse
import json
from pathlib import Path

import numpy as np

from CompareBuffers import normal_agreement_diagnostics, validate_capture_pair
from CameraBuffers import coincident_pixel_centres, native_pixel_coordinates
from NormalBuffers import angle_degrees, native_world_normals, remix_world_normals
from ReadBuffers import read_dds


def sample_points(values, x, y, bilinear=False):
    """Coordinates are native pixel indices; reject edges rather than wrapping."""
    height, width = values.shape[:2]
    if bilinear:
        ix, iy = np.floor(x).astype(int), np.floor(y).astype(int)
        if np.any((ix < 0) | (iy < 0) | (ix + 1 >= width) | (iy + 1 >= height)):
            raise ValueError("Sample outside image")
        fx, fy = x - ix, y - iy
        while fx.ndim < values[iy, ix].ndim:
            fx, fy = fx[..., None], fy[..., None]
        return ((1 - fy) * ((1 - fx) * values[iy, ix] + fx * values[iy, ix + 1]) +
                fy * ((1 - fx) * values[iy + 1, ix] + fx * values[iy + 1, ix + 1]))
    ix, iy = np.floor(x + .5).astype(int), np.floor(y + .5).astype(int)
    if np.any((ix < 0) | (iy < 0) | (ix >= width) | (iy >= height)):
        raise ValueError("Sample outside image")
    return values[iy, ix]


def fit_depth_shift(native_z, target_z, x, y, offsets):
    """All candidates use the identical depth pixels; normals are not an input."""
    candidates = []
    if not target_z.size or not np.all(np.isfinite(target_z) & (target_z > 0)):
        raise ValueError("Calibration depth must be nonempty, finite and positive")
    for dy in offsets:
        for dx in offsets:
            sampled = sample_points(native_z, x + dx, y + dy, bilinear=True)
            if not np.all(np.isfinite(sampled) & (sampled > 0)):
                raise ValueError("Invalid calibration depth; do not change the mask per candidate")
            relative = np.abs(sampled - target_z) / target_z
            candidates.append({"nativePixelShift": [float(dx), float(dy)],
                               "depthRelativeMAE": float(relative.mean()),
                               "depthRelativeP90": float(np.percentile(relative, 90))})
    if not candidates:
        raise ValueError("No candidate offsets")
    return sorted(candidates, key=lambda item: item["depthRelativeMAE"])


def probe(directory, require_matched_samples=False):
    native_metadata = json.loads(next(directory.glob("native-*.json")).read_text())
    remix_metadata = json.loads(next(directory.glob("rtx-*.json")).read_text())
    if not validate_capture_pair(native_metadata, remix_metadata):
        raise ValueError("This diagnostic requires a validated same-host-frame pair")
    if not native_metadata.get("grassLightingLoaded") or not remix_metadata.get("grassLightingLoaded"):
        raise ValueError("Grass Lighting must be loaded in both capture halves")
    native_depth, _ = read_dds(next(directory.glob("*-depth.dds")))
    target_depth, _ = read_dds(next(directory.glob("gbufferLinearZ_*.dds")))
    npixels, _ = read_dds(next(directory.glob("*-normalRoughness.dds")))
    normal_path = next(directory.glob("gbufferWorldNormals_*.dds"), None)
    rpixels, _ = read_dds(normal_path or next(directory.glob("worldNormals_*.dds")))
    classification, _ = read_dds(next(directory.glob("rtxImageDebugView_*.dds")))
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))
    if manifest.get("debugView") != 804:
        raise ValueError("Requires the debug804 grass classification capture")
    if manifest["cameraBefore"] != manifest["cameraAfter"]:
        raise ValueError("Camera moved during capture")
    bank_camera = dict(camX=13000, camY=-47300, camZ=650, camPitch=-.45000526309, camYaw=0)
    if any(abs(manifest["cameraBefore"].get(key, float("inf")) - value) > 1e-5
           for key, value in bank_camera.items()):
        raise ValueError("Fixed regions require the established Riverwood bank camera")
    if target_depth.shape[:2] not in ((720, 1280), (1080, 1920)) or native_depth.shape[:2] != (1080, 1920):
        raise ValueError("Bank diagnostic supports 720p or 1080p RTX and 1080p native")
    p = native_metadata["shadowProjection"]
    native_z = p[14] / (native_depth[..., 0] - p[10])
    target_z = target_depth[..., 0]
    native_normals = native_world_normals(npixels, native_metadata["shadowView"])
    target_normals = remix_world_normals(rpixels)
    yy, xx = np.indices(target_z.shape)
    height, width = target_z.shape
    x, y = (xx + .5) * 1920 / width - .5, (yy + .5) * 1080 / height - .5
    def region(x0, y0, x1, y1):
        return np.s_[round(y0 * height / 720):round(y1 * height / 720),
                     round(x0 * width / 1280):round(x1 * width / 1280)]
    # Fixed normalized calibration region, no depth agreement filtering.
    calibration = region(1100, 70, 1220, 140)
    ranked = fit_depth_shift(native_z, target_z[calibration], x[calibration], y[calibration],
                             np.arange(-2, 2.001, .125))
    dx, dy = ranked[0]["nativePixelShift"]
    coordinates = {"unregistered": (x, y), "depthFitted": (x + dx, y + dy)}
    camera_report = None
    depth_path = next(directory.glob("gbufferLinearZ_*.dds"))
    camera_path = Path(str(depth_path) + ".camera.json")
    if camera_path.exists():
        runtime = json.loads(camera_path.read_text())
        if runtime.get("depthBuffer") != depth_path.name:
            raise ValueError("Camera sidecar names another depth capture")
        ax, ay = native_pixel_coordinates(native_metadata, runtime, (height, width), (1080, 1920))
        coordinates["cameraRegistered"] = (ax, ay)
        camera_report = {"runtimeFrame": runtime["runtimeFrame"], "pixelJitter": runtime["pixelJitter"],
                         "coincidentPrimaryPixelCentres": coincident_pixel_centres(ax, ay, (1080, 1920)),
                         "nativePixelShiftMinMedianMax": [np.percentile(ax - x, [0, 50, 100]).tolist(),
                                                          np.percentile(ay - y, [0, 50, 100]).tolist()],
                         "scope": "Analytic ray projection from uploaded constants; nearest native sampling still quantizes subpixels"}
    if require_matched_samples and (not camera_report or not camera_report["coincidentPrimaryPixelCentres"]):
        raise ValueError("Actual primary pixel centres do not coincide; no matched-sample claim allowed")
    # Keep a fixed border valid for EVERY search candidate. No wrap/clamping.
    safe = (x >= 3) & (y >= 3) & (x < 1916) & (y < 1076)
    for sx, sy in coordinates.values():
        safe &= (sx >= 0) & (sy >= 0) & (sx <= 1919) & (sy <= 1079)
    cx = np.floor((xx + .5) * classification.shape[1] / width).astype(int)
    cy = np.floor((yy + .5) * classification.shape[0] / height).astype(int)
    rgb = classification[cy, cx, :3]
    grass = safe & (rgb[..., 1] > .9) & (rgb[..., 0] < .1) & (rgb[..., 2] < .1)
    masks = {"grassHeldOut": grass, "rockCalibration": np.zeros_like(safe),
             "groundHeldOut": np.zeros_like(safe), "stumpHeldOut": np.zeros_like(safe)}
    masks["rockCalibration"][calibration] = True
    masks["groundHeldOut"][region(145, 195, 225, 240)] = True
    masks["stumpHeldOut"][region(425, 218, 470, 233)] = True
    evaluations = {}
    for name, mask in masks.items():
        mask &= safe
        evaluations[name] = {}
        for label, (sx, sy) in coordinates.items():
            nx, ny = sx[mask], sy[mask]
            nz = sample_points(native_z, nx, ny)
            angles = angle_degrees(sample_points(native_normals, nx, ny), target_normals[mask])
            relative = np.abs(nz - target_z[mask]) / np.maximum(np.abs(nz), 1)
            matched = np.isfinite(relative) & (relative < .01)
            evaluations[name][label] = {
                "fixedSamples": int(mask.sum()),
                "nearestSampleChangedFraction": float(np.mean(
                    (np.floor(nx + .5) != np.floor(x[mask] + .5)) |
                    (np.floor(ny + .5) != np.floor(y[mask] + .5)))) if mask.any() else None,
                "allNormalAngleP50P90P99": np.percentile(angles, [50, 90, 99]).tolist() if angles.size else None,
                "depthRelativeP50P90P99": np.percentile(relative, [50, 90, 99]).tolist() if angles.size else None,
                "depthMatchedSamples": int(matched.sum()),
                "depthMatchedNormalAngleP50P90P99": np.percentile(angles[matched], [50, 90, 99]).tolist() if matched.any() else None,
            }
    matched_diagnostics = None
    if camera_report and camera_report["coincidentPrimaryPixelCentres"]:
        angles = angle_degrees(native_normals, target_normals)
        delta = target_z - native_z
        matched_diagnostics = normal_agreement_diagnostics(
            native_normals, target_normals, angles, grass, np.abs(delta))
        matched_diagnostics["scope"] = "Same primary pixel centres, fixed RTX grass mask; subsets diagnose causes, NOT parity gates"
        matched_diagnostics["signedDepthByNormalAgreement"] = {}
        for label, selected in (("under1Degree", grass & (angles < 1)), ("over30Degrees", grass & (angles > 30))):
            matched_diagnostics["signedDepthByNormalAgreement"][label] = {
                "samples": int(selected.sum()),
                "remixMinusNativeDepthP01P10P50P90P99": np.percentile(delta[selected], [1, 10, 50, 90, 99]).tolist() if selected.any() else None,
                "nativeGlossOneFraction": float(np.mean(npixels[..., 2][selected] >= .999)) if selected.any() else None,
            }
    return {"scope": "Analytic camera registration and separate rock-depth translation probe; NOT renderer parity",
            "pair": {key: native_metadata[key] for key in ("pid", "frame", "request")},
            "nativeSize": [1920, 1080], "remixSize": [width, height],
            "cameraRegistration": camera_report,
            "matchedGrassDiagnostics": matched_diagnostics,
            "normalBuffer": normal_path.name if normal_path else "legacy post-composite worldNormals",
            "calibration": {"boundsAt1280x720": [1100, 70, 1220, 140],
                            "pixels": int(target_z[calibration].size),
                            "sampling": "Bilinear depth fit; nearest unmodified normal/depth values for evaluation",
                            "searchBoundNativePixels": 2, "stepNativePixels": .125,
                            "bestAtSearchBoundary": abs(dx) == 2 or abs(dy) == 2,
                            "bestCandidates": ranked[:8],
                            "zeroShift": next(item for item in ranked if item["nativePixelShift"] == [0, 0])},
            "evaluations": evaluations}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-matched-samples", action="store_true")
    args = parser.parse_args()
    result = json.dumps(probe(args.directory, args.require_matched_samples), indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(result + "\n", encoding="utf-8")
    print(result)
