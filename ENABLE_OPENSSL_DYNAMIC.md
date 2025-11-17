# Building ClickHouse with Dynamic OpenSSL

This guide explains how to build ClickHouse with dynamic linking to system OpenSSL instead of building OpenSSL from source.

## Overview

When `ENABLE_OPENSSL_DYNAMIC=ON`, ClickHouse will:
- Link against the system's OpenSSL libraries (`libssl.so` and `libcrypto.so`)
- Allow dlopen() for OpenSSL provider modules (required for OpenSSL 3.x)
- Skip building OpenSSL from source
- Disable glibc-compatibility layer (incompatible with newer system glibc)
- Allow undefined symbols in shared libraries at link time

## Prerequisites

1. **System OpenSSL**: Your system must have OpenSSL development packages installed
   ```bash
   # Ubuntu/Debian
   sudo apt-get install libssl-dev

   # Fedora/RHEL
   sudo dnf install openssl-devel
   ```

2. **Sysroot Setup**: OpenSSL headers must be copied into the sysroot for compilation

## Build Steps

### 1. Copy OpenSSL Headers to Sysroot

The build uses `--sysroot` for hermetic compilation, so system headers must be copied into the sysroot:

```bash
# Set your sysroot path
SYSROOT="contrib/sysroot/linux-x86_64/x86_64-linux-gnu/libc"

# Create OpenSSL include directory
mkdir -p "${SYSROOT}/usr/include/openssl"

# Copy main OpenSSL headers
cp -r /usr/include/openssl/* "${SYSROOT}/usr/include/openssl/"

# Copy architecture-specific OpenSSL headers
ARCH_INCLUDE="/usr/include/x86_64-linux-gnu/openssl"
if [ -d "${ARCH_INCLUDE}" ]; then
    mkdir -p "${SYSROOT}/usr/include/x86_64-linux-gnu/openssl"
    cp -r ${ARCH_INCLUDE}/* "${SYSROOT}/usr/include/x86_64-linux-gnu/openssl/"
fi
```

**Important**: Do NOT copy the library files (`.so`) into the sysroot. The build uses full paths to system libraries.

### 2. Configure CMake

```bash
mkdir -p build
cd build

cmake .. \
    -DENABLE_OPENSSL_DYNAMIC=ON \
    -DENABLE_GLIBC_COMPATIBILITY=OFF
```

Notes:
- `ENABLE_GLIBC_COMPATIBILITY=OFF` is automatically set when `ENABLE_OPENSSL_DYNAMIC=ON`
- The build will use system glibc instead of the compatibility layer

### 3. Build

```bash
cmake --build build -j$(nproc)
```

## Architecture-Specific Notes

### x86_64
The architecture-specific headers are typically in `/usr/include/x86_64-linux-gnu/openssl/`. Adjust the path for your architecture:
- **aarch64**: `/usr/include/aarch64-linux-gnu/openssl/`
- **ppc64le**: `/usr/include/powerpc64le-linux-gnu/openssl/`
- **s390x**: `/usr/include/s390x-linux-gnu/openssl/`

## Technical Details

### Changes Made to Support Dynamic OpenSSL

1. **CMakeLists.txt**:
   - Moved `ENABLE_OPENSSL_DYNAMIC` option definition before `global-group` creation
   - Added `ENABLE_OPENSSL_DYNAMIC` as a preprocessor definition
   - Disabled `GLIBC_COMPATIBILITY` when dynamic OpenSSL is enabled
   - Changed linker flags from `--no-undefined` to `--allow-shlib-undefined`

2. **contrib/openssl-cmake/CMakeLists.txt**:
   - Added early return when `ENABLE_OPENSSL_DYNAMIC=ON`
   - Created INTERFACE IMPORTED targets for OpenSSL libraries
   - Used full paths to system libraries (`${OPENSSL_SYSTEM_LIB_DIR}/libssl.so`)

3. **contrib/delta-kernel-rs-cmake/CMakeLists.txt**:
   - Updated Rust FFI to use system OpenSSL paths

4. **programs/main.cpp** and **programs/keeper/keeper_main.cpp**:
   - Conditionally disabled dlopen() stubs when `ENABLE_OPENSSL_DYNAMIC` is defined
   - Required for OpenSSL 3.x to load provider modules (e.g., legacy.so)

5. **Deprecation Warning Fixes**:
   - Added `-Wno-deprecated-declarations` to Arrow and Parquet targets
   - Added pragma wrappers around PostgreSQL libpqxx deprecated functions
   - Added pragma wrappers around Azure SDK deprecated APIs

### Why These Changes Are Necessary

1. **glibc Compatibility**: System OpenSSL is compiled against system glibc (e.g., GLIBC_2.34, GLIBC_2.38). ClickHouse's glibc-compatibility layer provides older symbols, causing link failures. Solution: disable glibc-compatibility.

2. **Undefined Symbols**: With `--sysroot`, the linker can't resolve system glibc symbols that OpenSSL needs. Solution: use `--allow-shlib-undefined` to defer resolution to runtime.

3. **dlopen Blocking**: ClickHouse overrides dlopen() to return NULL for security. OpenSSL 3.x needs dlopen() to load provider modules. Solution: conditionally disable the override.

4. **Sysroot Headers**: The `--sysroot` flag isolates compilation from system headers. Solution: copy OpenSSL headers into sysroot.

## Runtime Considerations

When running ClickHouse built with `ENABLE_OPENSSL_DYNAMIC=ON`:

1. The system must have the same OpenSSL version (or compatible) that was used during build
2. The binary is NOT portable to systems with older glibc or different OpenSSL versions
3. OpenSSL provider modules must be available (typically in `/usr/lib/x86_64-linux-gnu/ossl-modules/`)

## Troubleshooting

### Error: "Failed to load OpenSSL 'legacy' provider"
This means dlopen() is still blocked. Ensure `ENABLE_OPENSSL_DYNAMIC` is defined as a preprocessor macro.

### Link Error: "undefined reference: pthread_once@GLIBC_2.34"
This means `--allow-shlib-undefined` is not set. Check that `ENABLE_OPENSSL_DYNAMIC=ON` is properly configured.

### Compilation Error: "openssl/opensslconf.h: No such file or directory"
OpenSSL headers are not in the sysroot. Follow step 1 to copy headers.

### Runtime Error: "symbol lookup error: undefined symbol"
The system's glibc or OpenSSL version differs from the build environment. This build configuration is not portable.

## When to Use Dynamic OpenSSL

Use `ENABLE_OPENSSL_DYNAMIC=ON` when:
- You need to use a specific system OpenSSL version (e.g., for compliance)
- You want to reduce binary size by using shared libraries
- You're building for a controlled environment with fixed system libraries

Do NOT use it when:
- You need portable binaries that run on multiple Linux distributions
- You need to support older glibc versions
- You're building for production release

## Reverting to Static OpenSSL

To revert to static OpenSSL linking:
1. Remove the build directory: `rm -rf build`
2. Configure without the flag: `cmake .. -DENABLE_OPENSSL_DYNAMIC=OFF`
3. Build normally: `cmake --build build`
