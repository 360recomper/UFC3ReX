"""Fetch the native renderer's third-party sources at their pinned commits and apply our patches.

    python -I tools/nr_fetch_third_party.py [--from-dir DIR]

Clones plume and reblue-XenosRecomp (with submodules) into third_party/, checks out the pins and
applies patches/plume.patch, patches/plume-gpu-pick.patch (NR-12c adapter pick), patches/plume-texture-pool.patch (NR-14d), patches/xenosrecomp.patch (SvR 2010's), patches/xenosrecomp-ufc.patch, patches/xenosrecomp-ufc2.patch, patches/xenosrecomp-ufc3.patch, patches/xenosrecomp-ufc4.patch, patches/xenosrecomp-ufc5.patch and patches/xenosrecomp-ufc6.patch
(UFC's shaders on top, NR-7). A folder that already holds the right
commit is only reset and re-patched. --from-dir takes the clones from a local folder that has
plume/ and XenosRecomp/ (e.g. another recomp's third_party) instead of GitHub; it is copied, so
the submodules come along without a network fetch.

Only needed for builds with UFC3_NATIVE_RENDERER=ON (build.bat native). Pins: CREDITS.md.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPOS = [
    # (folder, url, commit, patches applied in order)
    ("plume", "https://github.com/zolaware/plume.git", "e0c8871", ["plume.patch", "plume-gpu-pick.patch", "plume-texture-pool.patch"]),
    ("XenosRecomp", "https://github.com/zolaware/reblue-XenosRecomp.git", "339af41",
     ["xenosrecomp.patch", "xenosrecomp-ufc.patch", "xenosrecomp-ufc2.patch", "xenosrecomp-ufc3.patch", "xenosrecomp-ufc4.patch",
      "xenosrecomp-ufc5.patch", "xenosrecomp-ufc6.patch"]),
]


def git(*args, cwd=None):
    subprocess.run(["git", "-c", "core.longpaths=true", *args], cwd=cwd, check=True)


def head(path):
    r = subprocess.run(["git", "-C", str(path), "rev-parse", "--short=7", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--from-dir", type=Path, help="local folder with plume/ and XenosRecomp/")
    a = ap.parse_args()
    for name, url, commit, patches in REPOS:
        dest = ROOT / "third_party" / name
        if not dest.exists():
            if a.from_dir:
                print(f"copying {a.from_dir / name} -> {dest}")
                shutil.copytree(a.from_dir / name, dest, symlinks=True)
            else:
                git("clone", url, str(dest))
        if head(dest) != commit:
            git("-C", str(dest), "checkout", commit)
            git("-C", str(dest), "submodule", "update", "--init", "--recursive")
        # Drop any earlier (or foreign) patch, then apply ours.
        git("-C", str(dest), "checkout", "--", ".")
        for patch in patches:
            git("-C", str(dest), "apply", str(ROOT / "patches" / patch))
        print(f"{name}: {commit} + {', '.join('patches/' + p for p in patches)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
