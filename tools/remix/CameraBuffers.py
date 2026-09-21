"""Analytic native/RTX ray registration using captured projection constants."""

import numpy as np


def coincident_pixel_centres(x, y, native_shape):
    """Geometric sample agreement only, not texture coverage or normal parity."""
    if x.shape != native_shape or y.shape != native_shape:
        return False
    yy, xx = np.indices(native_shape)
    return bool(np.all(np.isfinite(x) & np.isfinite(y)) and
                np.max(np.abs(x - xx)) < 1e-3 and np.max(np.abs(y - yy)) < 1e-3)


def native_pixel_coordinates(native, runtime, target_shape, native_shape):
    """Return native pixel-index coordinates, with no fit to image contents.

    Matrices are column-major column-vector transforms. Skyrim's row-major
    row-vector memory has the same layout. Restore the recorded native rebasing
    origin before verifying that both captures use the same view transform.
    """
    if runtime.get("schema") != 1 or runtime.get("source") != "uploaded-raytrace-args":
        raise ValueError("Unsupported runtime camera metadata")
    if runtime.get("matrixLayout") != "column-major":
        raise ValueError("Unsupported camera matrix layout")
    height, width = target_shape
    nh, nw = native_shape
    if runtime.get("resolution") != [width, height] or runtime.get("depthExtent") != [width, height]:
        raise ValueError("Runtime camera/depth resolution mismatch")

    def matrix(values):
        # Both sources store float32; nine JSON digits round-trip that type,
        # not float64. Restore the original values before high-precision math.
        result = np.asarray(values, dtype=np.float32).astype(np.float64).reshape(4, 4).T
        if not np.isfinite(result).all():
            raise ValueError("Nonfinite camera matrix")
        return result

    nview = matrix(native["shadowView"])
    rview = matrix(runtime["worldToView"])
    if "shadowOrigin" in native:
        origin = np.asarray(native["shadowOrigin"], dtype=np.float64)
        if origin.shape != (3,) or not np.isfinite(origin).all():
            raise ValueError("Invalid native rebasing origin")
        # Match RestoreWorldViewTranslation: double dot products, float result.
        nview[:, 3] = (nview[:, 3] - nview[:, :3] @ origin).astype(np.float32)
    if not np.allclose(nview, rview, atol=1e-5, rtol=0):
        raise ValueError("Different view transforms; rebasing/depth-aware reprojection required")
    nproj = matrix(native["shadowProjection"])
    rproj = matrix(runtime["viewToProjectionJittered"])
    rinverse = matrix(runtime["projectionToViewJittered"])
    if not np.allclose(rproj @ rinverse, np.eye(4), atol=2e-5, rtol=0):
        raise ValueError("Runtime projection/inverse mismatch")
    yy, xx = np.indices((height, width), dtype=np.float64)
    clip = np.stack((2 * (xx + .5) / width - 1, 1 - 2 * (yy + .5) / height,
                     np.ones_like(xx), np.ones_like(xx)), axis=-1)
    native_clip = clip @ (nproj @ rinverse).T
    if np.any(np.abs(native_clip[..., 3]) < 1e-10):
        raise ValueError("Degenerate projected rays")
    ndc = native_clip[..., :2] / native_clip[..., 3:4]
    return (ndc[..., 0] + 1) * nw / 2 - .5, (1 - ndc[..., 1]) * nh / 2 - .5
