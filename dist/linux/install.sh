#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_DIR="$SCRIPT_DIR/cxxe-x86_64"

echo "Installing CXXE..."

sudo mkdir -p /usr/local/lib/cxxe

sudo cp "$SOURCE_DIR/cxxe" /usr/local/bin/cxxe
sudo chmod +x /usr/local/bin/cxxe

sudo cp -r "$SOURCE_DIR/stde" "$SOURCE_DIR/CMake" "$SOURCE_DIR/Examples" /usr/local/lib/cxxe/

echo "CXXE installed successfully!"
echo "Run 'cxxe --version' to verify the installation."