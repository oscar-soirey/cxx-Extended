#!/bin/bash

set -e

INSTALL_DIR="/usr/local/lib/cxxe"

echo "Installing CXXE..."

sudo mkdir -p "$INSTALL_DIR"
sudo cp -r cxxe "$INSTALL_DIR/"
sudo cp -r stde "$INSTALL_DIR/"

sudo ln -sf "$INSTALL_DIR/cxxe" /usr/local/bin/cxxe

echo "CXXE installed successfully."