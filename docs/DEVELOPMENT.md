# Development and build environments

Current application version: **UxAir 1.2.0, unreleased**. Security release hold applies; build/validation commands do not authorize installation, artifact distribution, merging or publication. NDL remains physically unvalidated for this release.

## Supported hosts and inputs

Use Linux x86-64 (Ubuntu 24.04, including a supported WSL2 Ubuntu installation) or macOS ARM64. Native Windows and Linux AArch64 are not supported cross-build hosts. `tools/bootstrap.py` selects the matching checksum-pinned SDK in `dependencies.lock.json`: Linux `openlgtv/buildroot-nc4` webos-d7ed7ee, or macOS `webosbrew/native-toolchain` webos-d7ed7ee.6. Do not substitute a generic ARM compiler or newer glibc sysroot. Keep WSL checkouts in the Linux filesystem. WSL is unavailable on the owner's currently inspected Windows host; CI provides the Linux build environment.

The ARM target is 32-bit ARMv7 EABI soft-float, with GLIBC <=2.19 and a private compatible libstdc++. SDL2, SDL2_ttf, Luna and the selected media APIs remain TV-provided. No system libraries are replaced by the IPK. SDK and native dependency archives are hash-pinned; UxPlay is commit-pinned. These controls are not independent attestation or a claim of byte-for-byte reproducibility across runner images.

On Ubuntu install the host prerequisites:

```sh
sudo apt-get update
sudo apt-get install --yes build-essential cmake ninja-build pkg-config perl curl git python3 file xz-utils bzip2 ffmpeg libsdl2-dev libsdl2-ttf-dev libsdl2-image-2.0-0 fonts-dejavu-core libavcodec-dev libavutil-dev libswresample-dev
```

For macOS ARM64 use Xcode command-line tools and suitable CMake/Ninja, pkg-config, SDL2/TTF/image and FFmpeg host development packages. The same pinned target dependency build is used. Do not infer newer-TV runtime compatibility from a successful macOS cross-build.

## Locked packaging tools

Use Node >=20 and the checked-in CLI 3.2.6 dependency graph; disable lifecycle scripts. No global npm install is needed.

```sh
npm ci --prefix tools/webos-cli --ignore-scripts --no-audit --no-fund
export PATH="$PWD/tools/webos-cli/node_modules/.bin:$PATH"
ares-package --version
```

The CLI's chmod postinstall is unnecessary for packaging and is deliberately disabled. Keep the lock unchanged during validation. Installed node_modules are ignored and excluded from corresponding-source archives; the package/lock manifests are included. If the CLI is absent, `tools/make-ipk.py` is the existing offline packaging fallback. Both paths apply the same internal permission policy and package validation; renewed physical acceptance is still required for a changed candidate.

## Native sanitizer tests

```sh
python3 tools/validate-metadata.py
tools/test.sh
c++ -std=c++11 -g -Wall -Wextra -fsanitize=address,undefined -Isrc tests/b7_storage_layout_test.cpp src/settings.cpp src/usage_stats.cpp -o build/tests/b7-storage-layout-test
sudo build/tests/b7-storage-layout-test
```

The root fixture models different owner/execution UIDs; it does not emulate firmware jail mounts. The full suite includes real generated allocation guards, private storage, pairing, queue/epoch handling, RAOP lifecycle, HEVC split headers/reopens, UI and IPK/source packaging. `tools/test.sh --diagnostics-only` is a shorter diagnostic subset, not release validation.

## Two explicit ARM build targets

Both packages are built from the same source commit. The backend is selected at compile/link time. There is no runtime backend switch or universal IPK.

Starfish + ACB, webOS 3-4:

```sh
tools/build.sh --media=starfish
python3 tools/verify-elf.py build/app
python3 tools/validate-ipk.py
```

Output: `dist/com.github.depius.uxair_1.2.0_arm-webos3-4.ipk`. It depends on libplayerAPIs/ACB and must not acquire an NDL dependency.

NDL DirectMedia, webOS 5+ (**physical validation pending**):

```sh
tools/build.sh --media=ndl
python3 tools/verify-elf.py build/app
python3 tools/validate-ipk.py
```

Output: `dist/com.github.depius.uxair_1.2.0_arm-webos5plus.ipk`. It links the NDL backend instead of Starfish/ACB. `tools/build.sh` currently defaults to ndl; always use the explicit option for review/release work. Each build refreshes backend/variant markers and staging. Validate that target immediately before building the next one.

Then, for an authorized candidate build:

```sh
tools/source-package.py
sha256sum dist/*.ipk dist/*-source.tar.gz
```

On macOS use `shasum -a 256` instead of sha256sum. Source archive: `dist/uxair-webos-1.2.0-source.tar.gz`. Record the exact source commit, build host, dependency lock and both checksums. The offline source archive lacks Git metadata; release verification should start from the exact repository commit so embedded build-info can be attributed correctly.

CI runs full host sanitizers and a separate ARM matrix for starfish/ndl, ELF/IPK checks, source packaging and checksums. **It uploads no candidate artifacts while the hold is active.** CI is not physical hardware acceptance. See [RELEASING.md](RELEASING.md) and [SECURITY_HARDENING.md](SECURITY_HARDENING.md).
