#!/usr/bin/env bash
set -ex
DEPS_DIR=$(dirname ${BASH_SOURCE[0]})
cd "$DEPS_DIR"
. common

VERSION=4.8.0
OPENCV_DIR="opencv-$VERSION"

# Check if OpenCV is already installed
if [ -f "$INSTALL_DIR/$HOST/lib/libopencv_world480.a" ]; then
    echo "OpenCV $VERSION is already installed"
    ls -l "$INSTALL_DIR/$HOST/lib"/libopencv_*.a "$INSTALL_DIR/$HOST/bin"/*.dll || true
    exit 0
fi

cd "$SOURCES_DIR"

# Clone OpenCV if not already present
if [ ! -d "$OPENCV_DIR" ]; then
    git clone --depth 1 --branch $VERSION https://github.com/opencv/opencv.git "$OPENCV_DIR"
fi

cd "$OPENCV_DIR"

# Create toolchain file
cat > mingw64_toolchain.cmake << EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

# Create and enter build directory
mkdir -p build
cd build

# Configure with CMake for MinGW cross-compilation
cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/$HOST" \
      -DCMAKE_TOOLCHAIN_FILE=../mingw64_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_WITH_STATIC_CRT=ON \
      -DBUILD_EXAMPLES=OFF \
      -DBUILD_TESTS=OFF \
      -DBUILD_PERF_TESTS=OFF \
      -DBUILD_opencv_apps=OFF \
      -DBUILD_opencv_python2=OFF \
      -DBUILD_opencv_python3=OFF \
      -DWITH_CUDA=OFF \
      -DENABLE_PRECOMPILED_HEADERS=OFF \
      -DWITH_IPP=OFF \
      -DWITH_OPENCL=OFF \
      -DBUILD_opencv_world=ON \
      -DCMAKE_CXX_FLAGS="-static-libgcc -static-libstdc++ -static" \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
      ..

# Build using all available cores
make -j$(nproc)

# Install to the specified prefix
make install

# Create bin directory if it doesn't exist
mkdir -p "$INSTALL_DIR/$HOST/bin"

# Create pkg-config file
mkdir -p "$INSTALL_DIR/$HOST/lib/pkgconfig"
cat > "$INSTALL_DIR/$HOST/lib/pkgconfig/opencv4.pc" << EOF
prefix=$INSTALL_DIR/$HOST
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include/opencv2
bindir=\${prefix}/bin

Name: opencv4
Description: Open Source Computer Vision Library
Version: $VERSION
Libs: -L\${libdir} -lopencv_world480
Cflags: -I\${includedir}
EOF

echo "OpenCV $VERSION has been successfully built and installed"

# Move opencv2 headers from nested location to direct include path
mv "$INSTALL_DIR/$HOST/include/opencv4/opencv2" "$INSTALL_DIR/$HOST/include/" && rm -r "$INSTALL_DIR/$HOST/include/opencv4"
