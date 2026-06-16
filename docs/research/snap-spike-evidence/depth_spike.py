"""
Snap-to-Surface feasibility spike.
Q: Can off-the-shelf monocular depth (Depth-Anything-V2-Large), run locally,
   turn a single photo of a projection surface into a usable surface estimate
   (clean depth + a fittable dominant plane + a boundary) in pre-show time?

Pipeline: image -> DA-V2 depth -> back-project to point cloud (assumed FOV) ->
RANSAC dominant-plane fit -> planarity residual + inlier mask (the "surface").
Reports timing for each stage. Saves depth + plane-inlier visualizations.

CAVEAT (reported, not hidden): DA-V2 'vitl' is RELATIVE depth (affine-ambiguous:
unknown global scale & shift). A real metric plane is only exactly planar in
metric depth; in relative depth it can bow. We measure planarity anyway as a
qualitative signal and flag where Metric3D (also on disk) would be the right tool.
"""
import sys, os, time, glob
import numpy as np
import cv2

SRC = r"D:\aiTools\ComfyUI_windows_portable\ComfyUI\custom_nodes\comfyui_controlnet_aux\src"
CKPT = r"D:\aiTools\ComfyUI_windows_portable\ComfyUI\custom_nodes\comfyui_controlnet_aux\ckpts\depth-anything\Depth-Anything-V2-Large\depth_anything_v2_vitl.pth"
sys.path.insert(0, SRC)
import torch
from custom_controlnet_aux.depth_anything_v2.dpt import DepthAnythingV2

OUT = r"C:\tmp\snap-spike\out"
os.makedirs(OUT, exist_ok=True)
DEVICE = "cuda" if torch.cuda.is_available() else "cpu"


def load_model():
    t = time.time()
    m = DepthAnythingV2(encoder="vitl", features=256, out_channels=[256, 512, 1024, 1024])
    sd = torch.load(CKPT, map_location="cpu")
    m.load_state_dict(sd)
    m = m.to(DEVICE).eval()
    return m, time.time() - t


def fit_plane_ransac(pts, iters=400, thresh=None, rng=None):
    """pts: Nx3. Returns (normal, d, inlier_mask, rms_inlier)."""
    if rng is None:
        rng = np.random.default_rng(0)
    n = len(pts)
    if thresh is None:
        thresh = 0.01 * (pts[:, 2].max() - pts[:, 2].min())  # 1% of depth span
    best_inl, best_plane = None, None
    for _ in range(iters):
        idx = rng.choice(n, 3, replace=False)
        p0, p1, p2 = pts[idx]
        nrm = np.cross(p1 - p0, p2 - p0)
        ln = np.linalg.norm(nrm)
        if ln < 1e-9:
            continue
        nrm = nrm / ln
        d = -nrm.dot(p0)
        dist = np.abs(pts.dot(nrm) + d)
        inl = dist < thresh
        if best_inl is None or inl.sum() > best_inl.sum():
            best_inl, best_plane = inl, (nrm, d)
    # refit on inliers via SVD
    P = pts[best_inl]
    c = P.mean(0)
    _, _, Vt = np.linalg.svd(P - c)
    nrm = Vt[-1]
    d = -nrm.dot(c)
    dist = np.abs(pts.dot(nrm) + d)
    inl = dist < thresh
    rms = np.sqrt((dist[inl] ** 2).mean())
    return nrm, d, inl, rms, thresh


def analyze(img_path, model):
    name = os.path.splitext(os.path.basename(img_path))[0]
    bgr = cv2.imread(img_path)
    if bgr is None:
        print(f"  !! could not read {img_path}")
        return None
    H, W = bgr.shape[:2]
    # depth
    t = time.time()
    depth = model.infer_image(bgr, input_size=518)  # HxW float
    torch.cuda.synchronize() if DEVICE == "cuda" else None
    t_inf = time.time() - t

    dmin, dmax = float(depth.min()), float(depth.max())
    # DA-V2 relative: larger value = NEARER (inverse-depth-like). Convert to a
    # monotonic "distance" proxy so a wall (roughly equidistant) reads as a plane.
    # z_proxy = (dmax - depth): near->small, far->large. Affine-ambiguous; fine for planarity.
    z = (dmax - depth).astype(np.float64)

    # back-project with an assumed pinhole (no EXIF intrinsics): ~62deg horiz FOV
    f = 0.5 * W / np.tan(np.deg2rad(62) / 2.0)
    cx, cy = W / 2.0, H / 2.0
    ys, xs = np.mgrid[0:H, 0:W]
    # subsample for speed
    step = max(1, int(np.sqrt((H * W) / 60000)))
    xs_s, ys_s, z_s = xs[::step, ::step], ys[::step, ::step], z[::step, ::step]
    Z = z_s.ravel()
    X = (xs_s.ravel() - cx) * Z / f
    Y = (ys_s.ravel() - cy) * Z / f
    pts = np.stack([X, Y, Z], 1)

    t = time.time()
    nrm, d, inl, rms, thresh = fit_plane_ransac(pts)
    t_fit = time.time() - t

    inl_frac = inl.mean()
    span = Z.max() - Z.min()
    rms_pct = 100.0 * rms / (span + 1e-9)

    # save depth colormap
    dn = ((depth - dmin) / (dmax - dmin + 1e-9) * 255).astype(np.uint8)
    cv2.imwrite(os.path.join(OUT, f"{name}_depth.png"), cv2.applyColorMap(dn, cv2.COLORMAP_TURBO))
    # save plane-inlier overlay (green = on dominant plane)
    inl_img = inl.reshape(z_s.shape)
    mask = cv2.resize(inl_img.astype(np.uint8) * 255, (W, H), interpolation=cv2.INTER_NEAREST)
    overlay = bgr.copy()
    overlay[mask > 0] = (0.45 * overlay[mask > 0] + 0.55 * np.array([0, 255, 0])).astype(np.uint8)
    cv2.imwrite(os.path.join(OUT, f"{name}_plane.png"), overlay)

    print(f"  [{name}] {W}x{H}  inf={t_inf*1000:.0f}ms  fit={t_fit*1000:.0f}ms")
    print(f"    plane inliers={inl_frac*100:.1f}%  planarity RMS={rms_pct:.2f}% of depth-span")
    return dict(name=name, W=W, H=H, t_inf=t_inf, t_fit=t_fit,
                inl_frac=inl_frac, rms_pct=rms_pct)


def main():
    print(f"device={DEVICE}  torch={torch.__version__}")
    model, t_load = load_model()
    print(f"model loaded in {t_load:.1f}s")
    imgs = sorted(glob.glob(r"C:\tmp\snap-spike\test_*.png") +
                  glob.glob(r"C:\tmp\snap-spike\test_*.jpg"))
    if not imgs:
        print("NO TEST IMAGES at C:\\tmp\\snap-spike\\test_*.png|jpg")
        return
    print(f"{len(imgs)} test image(s)")
    rows = []
    for p in imgs:
        # warm-up timing note: first infer includes CUDA kernel autotune
        r = analyze(p, model)
        if r:
            rows.append(r)
    print("\n=== SUMMARY ===")
    for r in rows:
        print(f"{r['name']:28s} inf={r['t_inf']*1000:6.0f}ms  inliers={r['inl_frac']*100:5.1f}%  RMS={r['rms_pct']:5.2f}%")


if __name__ == "__main__":
    main()
