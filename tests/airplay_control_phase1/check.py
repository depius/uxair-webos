"""Build and test Phase 1 against the exact public reference sources."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


GO_ATV = "f47a3ffcfa9458aac40562d73c314de1447df013"
IPHONE_MIRROR = "5e4443ceac899a3b0a7df4d92696cefc01673c6a"
SUBSET = "third_party/airplay-control"


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("go_atv", type=Path)
    parser.add_argument("iphone_mirror", type=Path)
    parser.add_argument("--go", default="go", help="Go 1.25+ executable")
    parser.add_argument("--cgo", action="store_true", help="include the reference C ABI build")
    parser.add_argument("--race", action="store_true", help="enable race detection for host tests")
    parser.add_argument("--arm-output", type=Path, help="also cross-build the Linux ARMv7 TCP tests here")
    parser.add_argument("--shared-output", type=Path, help="build the C ABI library instead of running tests")
    parser.add_argument("--reference-archive", type=Path, help="save the exact library source tar for redistribution")
    args = parser.parse_args()
    for repo, commit in ((args.go_atv, GO_ATV), (args.iphone_mirror, IPHONE_MIRROR)):
        if git(repo, "rev-parse", f"{commit}^{{commit}}") != commit:
            parser.error(f"missing pinned commit {commit}")
    for package in ("fairplay", "fpcontrol"):
        directory = f"internal/{package}"
        sender = git(args.go_atv, "rev-parse", f"{GO_ATV}:{directory}")
        receiver = git(args.iphone_mirror, "rev-parse", f"{IPHONE_MIRROR}:{SUBSET}/{directory}")
        if sender != receiver:
            parser.error(f"reference implementations differ in {directory}")
    if args.reference_archive:
        subprocess.run(
            ["git", "-C", str(args.iphone_mirror), "archive", "--format=tar",
             f"--output={args.reference_archive.resolve()}", IPHONE_MIRROR, SUBSET], check=True,
        )

    project = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="uxair-phase1-") as tmp:
        temp = Path(tmp)
        archive = temp / "reference.zip"
        subprocess.run(
            ["git", "-C", str(args.iphone_mirror), "archive", "--format=zip",
             f"--output={archive}", IPHONE_MIRROR, SUBSET],
            check=True,
        )
        with zipfile.ZipFile(archive) as source:
            source.extractall(temp)
        reference = temp / SUBSET
        env = os.environ.copy()
        for name in ("GOARM", "GOOS", "GOARCH"):
            env.pop(name, None)
        env.update(
            CGO_ENABLED="1" if args.cgo or args.race else "0",
            GOTOOLCHAIN="local",
            GOPROXY="off",
            GOFLAGS="",
            GOWORK=str(temp / "go.work"),
        )
        subprocess.run([args.go, "work", "init", str(project), str(reference)], cwd=temp, env=env, check=True)
        if args.shared_output:
            env["CGO_ENABLED"] = "1"
            if os.environ.get("GOARCH") == "arm":
                env.update(GOOS="linux", GOARCH="arm", GOARM="7,softfloat")
            subprocess.run(
                [args.go, "build", "-buildmode=c-shared", "-tags=netgo", "-trimpath", "-ldflags=-s -w",
                 "-o", str(args.shared_output.resolve()), "./cmd/shared"],
                cwd=project, env=env, check=True,
            )
            return
        flags = ["-race"] if args.race else []
        for module in (reference, project):
            subprocess.run([args.go, "test", "-count=1", "-v", *flags, "./..."], cwd=module, env=env, check=True)
        if args.arm_output:
            env.update(GOOS="linux", GOARCH="arm", GOARM="7,softfloat", CGO_ENABLED="0")
            subprocess.run(
                [args.go, "test", "-c", "-trimpath", "-o", str(args.arm_output.resolve()), "."],
                cwd=project, env=env, check=True,
            )


if __name__ == "__main__":
    main()
