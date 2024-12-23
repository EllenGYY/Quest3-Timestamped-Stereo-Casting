#!/usr/bin/env bash
set -ex
DEPS_DIR=$(dirname ${BASH_SOURCE[0]})
cd "$DEPS_DIR"
. common

VERSION="4.5.0"
AGORA_DIR="agora-$VERSION"

# Check if Agora SDK is already installed
if [ -f "$INSTALL_DIR/$HOST/lib/agora_rtc_sdk.dll" ]; then
    echo "Agora SDK $VERSION is already installed"
    ls -l "$INSTALL_DIR/$HOST/lib"/agora_*.dll "$INSTALL_DIR/$HOST/lib"/*.lib || true
    exit 0
fi

cd "$SOURCES_DIR"

# Create directory for Agora SDK
mkdir -p "$AGORA_DIR"
cd "$AGORA_DIR"

# Download SDK
echo "Downloading Agora SDK..."
curl -L "https://download.agora.io/sdk/release/Agora_Native_SDK_for_Windows_v${VERSION}_FULL.zip" -o agora_sdk.zip

# Extract SDK and check contents
echo "Extracting SDK..."
unzip -q agora_sdk.zip

# echo "Checking SDK contents:"
# find . -type f -name "*.h" -o -name "*.hpp"
# find . -type f -name "*.lib" -o -name "*.dll"

# Create necessary directories
mkdir -p "$INSTALL_DIR/$HOST/include/agora/low_level_api"
mkdir -p "$INSTALL_DIR/$HOST/include/agora/high_level_api"
mkdir -p "$INSTALL_DIR/$HOST/lib"
mkdir -p "$INSTALL_DIR/$HOST/bin"

# Copy header files
cp -r Agora_Native_SDK_for_Windows_FULL/sdk/low_level_api/* "$INSTALL_DIR/$HOST/include/agora/low_level_api/"
cp -r Agora_Native_SDK_for_Windows_FULL/sdk/high_level_api/* "$INSTALL_DIR/$HOST/include/agora/high_level_api/"

# Fix Windows.h include for cross-compilation
find "$INSTALL_DIR/$HOST/include/agora" -type f -name "*.h" -exec sed -i 's/<Windows.h>/<windows.h>/g' {} \;

# Copy library files
cp Agora_Native_SDK_for_Windows_FULL/sdk/x86_64/*.lib "$INSTALL_DIR/$HOST/lib/"
cp Agora_Native_SDK_for_Windows_FULL/sdk/x86_64/*.dll "$INSTALL_DIR/$HOST/lib/"
# Also copy DLLs to bin directory for runtime
cp Agora_Native_SDK_for_Windows_FULL/sdk/x86_64/*.dll "$INSTALL_DIR/$HOST/bin/"

# Create pkg-config file
mkdir -p "$INSTALL_DIR/$HOST/lib/pkgconfig"
cat > "$INSTALL_DIR/$HOST/lib/pkgconfig/agora.pc" << EOF
prefix=$INSTALL_DIR/$HOST
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include/agora
bindir=\${prefix}/bin

Name: agora
Description: Agora RTC SDK
Version: $VERSION
Libs: -L\${libdir} -lagora_rtc_sdk 
Cflags: -I\${includedir}
EOF

# Clean up
cd ..
rm -rf "$AGORA_DIR"

echo "Agora SDK $VERSION has been successfully installed" 