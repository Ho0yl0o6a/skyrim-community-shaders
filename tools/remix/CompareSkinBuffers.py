"""Strictly registered same-frame skin-buffer diagnostics, not a parity verdict."""

import argparse
import json
from pathlib import Path

import numpy as np

from CameraBuffers import coincident_pixel_centres, native_pixel_coordinates
from CompareBuffers import validate_capture_pair
from NormalBuffers import angle_degrees, native_world_normals, remix_world_normals
from ReadBuffers import read_dds


def skin_mask(classification):
    rgb = classification[..., :3]
    return (rgb[..., 1] > .9) & (rgb[..., 0] < .1) & (rgb[..., 2] < .1)


def authored_skin_normals(pixels):
    if pixels.shape[-1] != 4:
        raise ValueError("Authored normal diagnostic needs RGBA including the material mask")
    # Zero is the explicit non-skin value. Do not exclude nonunit or nonfinite
    # selected normals: those are errors to report, not pixels to hide.
    mask = (pixels[..., 3] > .9) & np.any(pixels[..., :3] != 0, axis=-1)
    normal = pixels[..., :3].astype(np.float64) * 2 - 1
    length = np.linalg.norm(normal, axis=-1)
    if not np.isfinite(pixels[mask]).all() or np.any(length[mask] < 1e-8):
        raise ValueError("Invalid selected authored normal")
    return mask, normal / np.maximum(length[..., None], 1e-8), length


def metrics(mask, native_normals, target_normals, native_z, target_z, native_rgb, target_rgb, configured_albedo_scale=None):
    if configured_albedo_scale is not None and (not np.isfinite(configured_albedo_scale) or configured_albedo_scale <= 0):
        raise ValueError("Configured albedo scale must be finite and positive")
    count = int(mask.sum())
    if not count:
        return {"pixels": 0}
    angles = angle_degrees(native_normals[mask], target_normals[mask])
    delta = target_z[mask] - native_z[mask]
    n, r = native_rgb[mask], target_rgb[mask]
    if not all(np.isfinite(value).all() for value in (angles, delta, n, r)):
        raise ValueError("Nonfinite values in the selected skin pixels")
    result = {
        "pixels": count,
        "normalAngleP50P90P99Max": np.percentile(angles, [50, 90, 99, 100]).tolist(),
        "normalOver5DegreeFraction": float(np.mean(angles > 5)),
        "normalOver30DegreeFraction": float(np.mean(angles > 30)),
        "signedDepthP01P10P50P90P99": np.percentile(delta, [1, 10, 50, 90, 99]).tolist(),
        "absoluteDepthP50P90P99": np.percentile(np.abs(delta), [50, 90, 99]).tolist(),
        "rawNativeAlbedoMAE": np.mean(np.abs(r - n), axis=0).tolist(),
        "pow22NativeAlbedoMAE": np.mean(np.abs(r - np.abs(n) ** 2.2), axis=0).tolist(),
    }
    if configured_albedo_scale is not None:
        difference = np.abs(r / configured_albedo_scale - np.abs(n) ** 2.2)
        result["configuredScaleDiagnostic"] = {
            "scale": configured_albedo_scale,
            "pow22NativeAlbedoMAE": np.mean(difference, axis=0).tolist(),
            "absoluteErrorP50P90P99": np.percentile(difference, [50, 90, 99], axis=0).tolist(),
            "targetClippedChannelFraction": np.mean(r >= 1, axis=0).tolist(),
        }
    return result


def compare(directory, configured_albedo_scale=None):
    native = json.loads(next(directory.glob("native-*.json")).read_text())
    target = json.loads(next(directory.glob("rtx-*.json")).read_text())
    if not validate_capture_pair(native, target):
        raise ValueError("Same-host-frame paired capture required")
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))
    debug_view = manifest.get("debugView")
    if debug_view not in (801, 815):
        raise ValueError("Debug801 diffusion mask or debug815 authored MSN normals required")
    if manifest["cameraBefore"] != manifest["cameraAfter"]:
        raise ValueError("Camera moved during capture")
    depth_path = next(directory.glob("gbufferLinearZ_*.dds"))
    runtime = json.loads(Path(str(depth_path) + ".camera.json").read_text())
    if runtime.get("depthBuffer") != depth_path.name:
        raise ValueError("Camera sidecar names another depth capture")
    nd, _ = read_dds(next(directory.glob("*-depth.dds")))
    rd, _ = read_dds(depth_path)
    x, y = native_pixel_coordinates(native, runtime, rd.shape[:2], nd.shape[:2])
    if not coincident_pixel_centres(x, y, nd.shape[:2]):
        raise ValueError("Primary pixel centres differ; restart with -MatchCaptureSamples")
    nn, _ = read_dds(next(directory.glob("*-normalRoughness.dds")))
    rn, _ = read_dds(next(directory.glob("gbufferWorldNormals_*.dds")))
    na, _ = read_dds(next(directory.glob("*-albedo.dds")))
    ra, _ = read_dds(next(directory.glob("gbufferAlbedo_*.dds")), remix_vulkan_packing=True)
    classification, _ = read_dds(next(directory.glob("rtxImageDebugView_*.dds")))
    if any(value.shape[:2] != nd.shape[:2] for value in (nn, rn, na, ra, classification)):
        raise ValueError("Normal, albedo, depth or classification extent differs")
    authored_normal = authored_length = None
    if debug_view == 815:
        mask, authored_normal, authored_length = authored_skin_normals(classification)
    else:
        mask = skin_mask(classification)
    if not mask.any():
        raise ValueError("No diffusion-profile pixels captured")
    nnormal = native_world_normals(nn, native["shadowView"])
    rnormal = remix_world_normals(rn)
    p = np.asarray(native["shadowProjection"], dtype=np.float32).astype(np.float64)
    nz = p[14] / (nd[..., 0] - p[10])
    rz = rd[..., 0]
    padded = np.pad(mask, 1, constant_values=False)
    h, w = mask.shape
    interior = np.logical_and.reduce([padded[dy:dy+h, dx:dx+w] for dy in range(3) for dx in range(3)])
    delta = np.abs(rz - nz)
    selections = {
        "allSkinMask": mask,
        "interior3x3": interior,
        "interiorDepthWithin1UnitDiagnostic": interior & (delta < 1),
        "interiorDepthWithinPoint1UnitDiagnostic": interior & (delta < .1),
    }
    yy, xx = np.indices(mask.shape)
    result = {
        "scope": "Same-frame, coincident primary rays, fixed RTX diffusion mask. No native material-ID mask; NOT a renderer parity verdict.",
        "colourScope": "Raw and power2.2 native albedo are separate hypotheses; no colour-domain assumption selected.",
        "configuredScaleScope": "Optional scale is supplied from known runtime configuration, never fitted. Raw metrics retained. Division cannot recover saturated values; not a parity verdict.",
        "debugView": debug_view,
        "maskScope": "Diffusion materials with a sampled model-space normal" if debug_view == 815 else "All diffusion materials",
        "subsetScope": "All-mask result is retained. Interior/depth subsets diagnose boundaries and surface mismatch, not acceptance gates.",
        "pair": {key: native[key] for key in ("pid", "frame", "request")},
        "camera": manifest["cameraBefore"],
        "pixelJitter": runtime["pixelJitter"],
        "maximumPrimaryDisplacementPixels": [float(np.max(np.abs(x-xx))), float(np.max(np.abs(y-yy)))],
        "metrics": {name: metrics(selected, nnormal, rnormal, nz, rz, na[..., :3], ra[..., :3], configured_albedo_scale) for name, selected in selections.items()},
    }
    if authored_normal is not None:
        result['authoredNormalMetrics'] = {
            name: metrics(selected, nnormal, authored_normal, nz, rz, na[..., :3], ra[..., :3], configured_albedo_scale)
            for name, selected in selections.items()
        }
        result['authoredNormalLengthP01P50P99'] = np.percentile(authored_length[mask], [1, 50, 99]).tolist()
        result['authoredToFinalNormalAngleP50P90P99Max'] = np.percentile(
            angle_degrees(authored_normal[mask], rnormal[mask]), [50, 90, 99, 100]).tolist()
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--configured-albedo-scale", type=float,
                        help="Known runtime albedo multiplier; optional diagnostic, never fitted from pixels")
    args = parser.parse_args()
    report = json.dumps(compare(args.directory, args.configured_albedo_scale), indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
    print(report)
