"""Signed distance shapes (in millimetres) and turning them into smooth meshes.

Negative inside. Every function takes the coordinates as numpy arrays, so the same
shape is sampled on a grid for marching cubes and at the mesh's vertices for normals
and colours.
"""

import numpy as np
from skimage import measure


def length2(a, b):
    return np.sqrt(a * a + b * b)


def round_rect(x, y, cx, cy, hx, hy, r):
    """A rounded rectangle in a plane: middle (cx, cy), half sizes, corner radius."""
    qx = np.abs(x - cx) - hx + r
    qy = np.abs(y - cy) - hy + r
    return length2(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def circle(x, y, cx, cy, r):
    return length2(x - cx, y - cy) - r


def extrude(d2, z, z0, z1, r=0.0):
    """A plane shape pulled from z0 to z1, its edges rounded by r."""
    dz = np.abs(z - (z0 + z1) / 2) - (z1 - z0) / 2
    wx, wy = d2 + r, dz + r
    return length2(np.maximum(wx, 0), np.maximum(wy, 0)) + np.minimum(np.maximum(wx, wy), 0) - r


def box(x, y, z, x0, x1, y0, y1, z0, z1, r=0.0):
    return extrude(round_rect(x, y, (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2, min(r, (x1 - x0) / 2, (y1 - y0) / 2)), z, z0, z1, r)


def smin(a, b, k):
    h = np.maximum(k - np.abs(a - b), 0) / k
    return np.minimum(a, b) - h * h * k / 4


def smax(a, b, k):
    return -smin(-a, -b, k)


def cut(a, b, k=0.6):
    """a with b taken out, the edge softened by k."""
    return smax(a, -b, k) if k > 0 else np.maximum(a, -b)


def mesh(f, lo, hi, step, keep=0.25):
    """Samples f on a grid and returns (vertices, faces, normals), with the triangles
    reduced to about `keep` of marching cubes' and the normals from f's gradient."""
    lo = np.asarray(lo, float) - 2 * step
    hi = np.asarray(hi, float) + 2 * step
    n = np.ceil((hi - lo) / step).astype(int) + 1
    ys = lo[1] + np.arange(n[1]) * step
    zs = lo[2] + np.arange(n[2]) * step
    vol = np.empty(n, np.float32)
    Y, Z = np.meshgrid(ys, zs, indexing="ij")
    for i in range(n[0]):
        X = np.full_like(Y, lo[0] + i * step)
        vol[i] = f(X, Y, Z)
    v, faces, _, _ = measure.marching_cubes(vol, 0.0, spacing=(step, step, step))
    v += lo
    if keep < 1:
        import fast_simplification

        v, faces = fast_simplification.simplify(v.astype(np.float32), faces.astype(np.int32), target_reduction=1 - keep)
    normals = gradient(f, v, step * 0.5)
    # Wind each triangle the way its normals point.
    a, b, c = v[faces[:, 0]], v[faces[:, 1]], v[faces[:, 2]]
    fn = np.cross(b - a, c - a)
    flip = np.einsum("ij,ij->i", fn, normals[faces].sum(axis=1)) < 0
    faces[flip] = faces[flip][:, ::-1]
    return v.astype(np.float32), faces.astype(np.uint32), normals.astype(np.float32)


def gradient(f, v, e):
    x, y, z = v[:, 0], v[:, 1], v[:, 2]
    g = np.stack([f(x + e, y, z) - f(x - e, y, z), f(x, y + e, z) - f(x, y - e, z), f(x, y, z + e) - f(x, y, z - e)], axis=1)
    return g / np.maximum(np.linalg.norm(g, axis=1, keepdims=True), 1e-9)


def flat_normals(v, faces):
    """Splits the vertices per triangle and gives each its face's normal (sharp edges)."""
    v = v[faces.reshape(-1)]
    tri = v.reshape(-1, 3, 3)
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    return v, np.arange(len(v), dtype=np.uint32).reshape(-1, 3), np.repeat(n, 3, axis=0)
