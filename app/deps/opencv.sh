#!/usr/bin/env bash
set -ex
DEPS_DIR=$(dirname ${BASH_SOURCE[0]})
cd "$DEPS_DIR"
. common

VERSION=4.8.0
FILENAME=opencv-$VERSION-windows.exe
SHA256SUM=1c8b1b78a51c46852eb5569d762bd11d0097a45c3db1f03eb41aba6f727b8942
cd "$SOURCES_DIR"

# Check if OpenCV DLLs are already installed
if [ -f "$INSTALL_DIR/$HOST/bin/opencv_world480.dll" ]; then
    echo "OpenCV DLLs already installed, skipping extraction"
    exit 0
fi

if [[ ! -f "$FILENAME" ]]; then
    # Download pre-built Windows binaries
    wget -O "$FILENAME" "https://github.com/opencv/opencv/releases/download/$VERSION/opencv-$VERSION-windows.exe"
    echo "$SHA256SUM $FILENAME" | sha256sum -c
fi

# Create a temporary directory for extraction
TEMP_DIR="opencv_extract"
mkdir -p "$TEMP_DIR"

# Extract the self-extracting exe using 7z
7z x "$FILENAME" -o"$TEMP_DIR"

# Debug: Print directory structure
echo "Directory structure:"
find "$TEMP_DIR" -type f -name "*.dll" -o -name "*.dll.a"

# Create necessary directories
mkdir -p "$INSTALL_DIR/$HOST/lib"
mkdir -p "$INSTALL_DIR/$HOST/include"
mkdir -p "$INSTALL_DIR/$HOST/bin"

# Copy the required files based on actual structure
cp -r "$TEMP_DIR"/opencv/build/include/* "$INSTALL_DIR/$HOST/include/"

# We'll update these paths based on the actual structure
find "$TEMP_DIR" -name "*.dll" -exec cp {} "$INSTALL_DIR/$HOST/bin/" \;

# Clean up
rm -rf "$TEMP_DIR"

# Create pkg-config directory if it doesn't exist
mkdir -p "$INSTALL_DIR/$HOST/lib/pkgconfig"

# Create pkg-config file for the OpenCV world library
cat > "$INSTALL_DIR/$HOST/lib/pkgconfig/opencv4.pc" << EOF
prefix=$INSTALL_DIR/$HOST
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: opencv4
Description: Open Source Computer Vision Library
Version: $VERSION
Libs: -L\${prefix}/bin -lopencv_world480
Cflags: -I\${includedir}
EOF

ls -l "$INSTALL_DIR/$HOST/bin"/opencv_*.dll || true