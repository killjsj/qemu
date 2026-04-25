#!/bin/bash
set -e  # 出错时立即退出

echo "Installing dependencies..."
sudo apt-get install -y build-essential meson ninja-build pkg-config zip \
                diffutils \
                python3 python3-venv python3-pip \
                libglib2.0-dev libusb-1.0-0-dev libncursesw5-dev \
                libpixman-1-dev libepoxy-dev libv4l-dev libpng-dev \
                libsdl2-dev libsdl2-image-dev libgtk-3-dev libgdk-pixbuf2.0-dev \
                libasound2-dev libpulse-dev \
                libx11-dev pkg-config
pip3 install tomli

ROOT_DIR="$PWD"
mkdir -p build && cd build

echo "Configuring and building QEMU..."
../configure --disable-werror --enable-sdl --enable-gtk \
    --target-list="x86_64-softmmu,aarch64-softmmu" --enable-debug || exit 1
make -j$(nproc) || exit 1

echo "Deploying QEMU and its dependencies..."

FIN_DIR="$ROOT_DIR/build/fin"
DONE_DIR="$ROOT_DIR/done_zip"
mkdir -p "$DONE_DIR"

TARGETS=("x86_64-softmmu" "aarch64-softmmu")

for target in "${TARGETS[@]}"; do
    arch=$(echo "$target" | cut -d'-' -f1)
    exe="qemu-system-$arch"

    if [[ ! -f "$exe" ]]; then
        echo "WARNING: $exe not found, skipping $target"
        continue
    fi

    # 清空并重建临时目录结构
    rm -rf "$FIN_DIR"
    mkdir -p "$FIN_DIR/bin/share"

    # 复制可执行文件到 bin/
    cp "$exe" "$FIN_DIR/bin/"
    if [[ $target == "aarch64-softmmu" ]]; then
        # arm64 可执行文件重命名为 qemu-system-arm64
        mv "$FIN_DIR/bin/$exe" "$FIN_DIR/bin/qemu-system-arm64"
    fi

    # 复制固件到 bin/share/
    cp -r ../pc-bios/* "$FIN_DIR/bin/share/"

    # 打包：压缩包内只包含 bin 目录
    (cd "$FIN_DIR" && zip -r "$DONE_DIR/linux-${target}.zip" bin)
done

echo "Done! Zip files are in $DONE_DIR"