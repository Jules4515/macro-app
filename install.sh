#!/usr/bin/env bash
set -e

echo "Installing Ubuntu dependencies..."
sudo apt-get update -y
sudo apt-get install -y build-essential pkg-config libgtk-3-dev libjson-c-dev libayatana-appindicator3-dev acl

echo "Building macro_app..."
make clean
make
chmod +x macro_app install.sh

echo "Configuring /dev/uinput permissions..."
sudo modprobe uinput 2>/dev/null || true
echo "uinput" | sudo tee /etc/modules-load.d/uinput.conf > /dev/null 2>&1 || true

cat << 'EOF' | sudo tee /etc/udev/rules.d/99-macro-app.rules > /dev/null
KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"
KERNEL=="event*", SUBSYSTEM=="input", TAG+="uaccess"
KERNEL=="uinput", GROUP="input", MODE="0660"
KERNEL=="event*", GROUP="input", MODE="0660"
EOF

sudo usermod -a -G input "$USER" 2>/dev/null || true
sudo udevadm control --reload-rules 2>/dev/null || true
sudo udevadm trigger 2>/dev/null || true
sudo setfacl -m "u:$USER:rw" /dev/uinput /dev/input/event* 2>/dev/null || true

echo "Done. Run: ./macro_app"
