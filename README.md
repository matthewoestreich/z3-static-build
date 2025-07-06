# z3-static-build

Automated static builds of [Z3 Theorem Prover](https://github.com/Z3Prover/z3) for multiple platforms using GitHub Actions.

## Overview

This repository provides prebuilt static libraries of Z3 for:

- Linux (x64)  
- macOS (x64 and arm64)  
- Windows (x64)  

Builds are generated automatically on GitHub Actions and published as [GitHub Releases](https://github.com/matthewoestreich/z3-static-build/releases).

Use these static libraries to link Z3 easily in your C or C++ projects without needing to build Z3 from source or worry about `.dll` files.

## Features

- Cross-platform static library builds using GitHub Actions  
- Uses the official Z3 source code, pinned to a specific version  
- Includes necessary headers for integration  
- Ready-to-use `.a` (static libs) or `.lib` (Windows) binaries  

## Usage

### Download Prebuilt Binaries

Go to the [Releases page](https://github.com/matthewoestreich/z3-static-build/releases) and download the appropriate archive for your platform.

### Integrate into Your Project

1. Extract the archive.  
2. Include headers from the `include/` directory.  
3. Link against the static library found in the `lib/<platform>/` directory.  

Example CMake snippet:

```cmake
include_directories(path/to/z3-static-build/include)
link_directories(path/to/z3-static-build/lib/<platform>)

add_executable(your_executable main.c)
target_link_libraries(your_executable PRIVATE z3)
```

## Build from Source

If you want to rebuild Z3 yourself, you can run the build script locally:

```bash
bash scripts/build.sh
```

*Note:* This requires Python, CMake, and a C++ compiler installed.

## GitHub Actions

Builds are triggered on pushes to the `main` branch and on manual dispatch via the GitHub Actions UI.

Artifacts are uploaded for each platform in the "archive" directory and published as releases. 

The "archive" directory is a folder named after the Z3 version, current platform, and current architecture. So if you were on `Linux x86_64` and building Z3 `v1.1.1` the "archive" folder would be: `z3-1.1.1-linux-x64`

## Create Release

```bash
# From root of this project
bash scripts/create_release.sh vX.X.X
# Where 'vX.X.X' is the tag (eg. v1.0.0)
```

## Delete Tag 

**This will delete both the remote and local tag**

If something went wrong during a build, you should delete 'that' tag before you try again.

```bash
# From root of this project
bash scripts/delete_tag.sh vX.X.X
# Where 'vX.X.X' is the tag (eg. v1.0.0)
```

⚠️ If there are any releases tied to the tag you just deleted, don't forget to manually remove the release!

## Licenses

Z3 [MIT License](https://github.com/Z3Prover/z3/blob/master/LICENSE.txt).

## References

- [Z3 GitHub repository](https://github.com/Z3Prover/z3)  
- [Z3 Documentation](https://z3prover.github.io/)
