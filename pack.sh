#!/bin/bash
# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Detect architecture and set proper paths
if [[ $(uname -m) == 'arm64' ]]; then
    # Apple Silicon
    HOMEBREW_PREFIX="/opt/homebrew"
else
    # Intel
    HOMEBREW_PREFIX="/usr/local"
fi

echo -e "${BLUE}📦 Creating macOS app bundle...${NC}"

# Ensure the app exists
if [ ! -f ".build/ned" ]; then
    echo -e "${RED}❌ Application not found. Run build.sh first.${NC}"
    exit 1
fi

# App bundle structure
APP_NAME="Ned"
APP_BUNDLE="$APP_NAME.app"
CONTENTS="$APP_BUNDLE/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"
FRAMEWORKS="$CONTENTS/Frameworks"
# Create directory structure
mkdir -p "$MACOS"
mkdir -p "$RESOURCES"
mkdir -p "$FRAMEWORKS"

echo "Copying resources..."
cp .build/ned "$MACOS/$APP_NAME"
cp -r fonts "$RESOURCES/"
cp -r icons "$RESOURCES/"
cp -r shaders "$RESOURCES/"
cp .ned.json "$RESOURCES/"



echo "Adding app icon..."
cp -r icons/ned.icns "$RESOURCES/ned.icns"

# Create Info.plist
cat > "$CONTENTS/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>$APP_NAME</string>
    <key>CFBundleIconFile</key>
    <string>ned.icns</string>
    <key>CFBundleIdentifier</key>
    <string>com.nealaggarwal.ned</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# Copy required libraries - with error handling
echo "Copying libraries..."

# Function to safely copy libraries with error handling
copy_lib() {
    local source="$1"
    local dest="$2"
    
    if [ -f "$source" ]; then
        cp "$source" "$dest"
        echo "✅ Copied: $source"
    else
        echo "⚠️  Warning: Could not find $source"
        # Try alternative locations
        local lib_name=$(basename "$source")
        local alt_paths=(
            "$HOMEBREW_PREFIX/lib/$lib_name"
            "/usr/local/lib/$lib_name"
            "/usr/lib/$lib_name"
        )
        
        for alt_path in "${alt_paths[@]}"; do
            if [ -f "$alt_path" ]; then
                cp "$alt_path" "$dest"
                echo "✅ Found alternative: $alt_path"
                return 0
            fi
        done
        echo "❌ Could not find $lib_name in any standard location"
    fi
}

# GLEW Library
GLEW_LIB="$HOMEBREW_PREFIX/Cellar/glew/2.2.0_1/lib/libGLEW.dylib"
copy_lib "$GLEW_LIB" "$FRAMEWORKS/"

# GLFW Library
GLFW_LIB="$HOMEBREW_PREFIX/Cellar/glfw/3.3.9/lib/libglfw.3.dylib"
copy_lib "$GLFW_LIB" "$FRAMEWORKS/"

echo "Fixing library paths..."
# Use install_name_tool to update dynamic library paths in the executable
install_name_tool -change "@rpath/libGLEW.dylib" "@executable_path/../Frameworks/libGLEW.dylib" "$MACOS/$APP_NAME" 2>/dev/null || true
install_name_tool -change "@rpath/libglfw.3.dylib" "@executable_path/../Frameworks/libglfw.3.dylib" "$MACOS/$APP_NAME" 2>/dev/null || true

#sign app
echo "Signing libraries..."
for lib in "$FRAMEWORKS"/*.dylib; do
    codesign --force --sign - "$lib"
done

echo "Signing executable..."
codesign --force --sign - "$MACOS/$APP_NAME"

echo "Signing app bundle..."
codesign --force --deep --sign - "$APP_BUNDLE"

# Create DMG
echo "Creating DMG..."
hdiutil create -volname "$APP_NAME" -srcfolder "$APP_BUNDLE" -ov -format UDZO "$APP_NAME.dmg"

echo -e "${GREEN}✅ Package created: $APP_NAME.dmg${NC}"