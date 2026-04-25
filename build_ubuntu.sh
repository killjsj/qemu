#!/bin/bash
set -e  # 可选：让脚本在出错时立刻退出

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
DONE_DIR="$ROOT_DIR/done_zip"          # 新增：最终输出目录
mkdir -p "$DONE_DIR"                  # 确保输出目录存在
mkdir -p "$FIN_DIR"                   # 确保临时打包目录存在（避免 rm 时无目录报错）

TARGETS=("x86_64-softmmu" "aarch64-softmmu")

for target in "${TARGETS[@]}"; do
    arch=$(echo "$target" | cut -d'-' -f1)
    dest="$FIN_DIR/$arch"

    # 清空临时目录，避免污染
    rm -rf "$FIN_DIR"/*
    mkdir -p "$dest"

    exe="qemu-system-$arch"
    echo "$exe"
    if [[ ! -f "$exe" ]]; then
        echo "WARNING: $exe not found, skipping $target"
        continue
    fi

    cp "$exe" "$dest/"
    if [[ $target == "aarch64-softmmu" ]]; then
        mv "$dest/$exe" "$dest/qemu-system-arm64"
    fi

    mkdir -p "$dest/share"
    cp -r ../pc-bios/* "$dest/share/"

    # 打包到 done_zip 目录
    (cd "$FIN_DIR" && zip -r "$DONE_DIR/linux-${target}.zip" *)
done

echo "Done! Zip files are in $DONE_DIR"