#!/bin/bash
set -e  # 出错即停

# Determine package prefix based on MSYSTEM
if [[ "$MSYSTEM" == "CLANGARM64" ]]; then
    PKG_PREFIX="mingw-w64-clang-aarch64"
    PKG_CONFIG_PATH="/clangarm64/lib/pkgconfig:/clangarm64/share/pkgconfig"
    TARGET_LIST="aarch64-softmmu"
    TARGETS=("aarch64-softmmu")
elif [[ "$MSYSTEM" == "UCRT64" ]]; then
    PKG_PREFIX="mingw-w64-ucrt-x86_64"
    PKG_CONFIG_PATH="/ucrt64/lib/pkgconfig:/ucrt64/share/pkgconfig"
    TARGET_LIST="x86_64-softmmu,aarch64-softmmu"
    TARGETS=("x86_64-softmmu" "aarch64-softmmu")
elif [[ "$MSYSTEM" == "MINGW64" ]]; then
    PKG_PREFIX="mingw-w64-x86_64"
    PKG_CONFIG_PATH="/mingw64/lib/pkgconfig:/mingw64/share/pkgconfig"
    TARGET_LIST="x86_64-softmmu,aarch64-softmmu"
    TARGETS=("x86_64-softmmu" "aarch64-softmmu")
else
    echo "Unsupported MSYSTEM: $MSYSTEM"
    exit 1
fi

export PKG_CONFIG_PATH

# 安装依赖（MSYS2 环境）
pacman -Sy --noconfirm ${PKG_PREFIX}-toolchain zip git
pacman -Sy --noconfirm ${PKG_PREFIX}-meson ${PKG_PREFIX}-ninja \
           ${PKG_PREFIX}-python \
           ${PKG_PREFIX}-python-sphinx \
           ${PKG_PREFIX}-python-sphinx_rtd_theme \
           ${PKG_PREFIX}-autotools \
           ${PKG_PREFIX}-cc \
           ${PKG_PREFIX}-angleproject \
           ${PKG_PREFIX}-capstone \
           ${PKG_PREFIX}-curl \
           ${PKG_PREFIX}-cyrus-sasl \
           ${PKG_PREFIX}-expat \
           ${PKG_PREFIX}-fontconfig \
           ${PKG_PREFIX}-freetype \
           ${PKG_PREFIX}-fribidi \
           ${PKG_PREFIX}-gcc-libs \
           ${PKG_PREFIX}-gdk-pixbuf2 \
           ${PKG_PREFIX}-gettext \
           ${PKG_PREFIX}-glib2 \
           ${PKG_PREFIX}-gmp \
           ${PKG_PREFIX}-gnutls \
           ${PKG_PREFIX}-graphite2 \
           ${PKG_PREFIX}-gst-plugins-base \
           ${PKG_PREFIX}-gstreamer \
           ${PKG_PREFIX}-gtk3 \
           ${PKG_PREFIX}-harfbuzz \
           ${PKG_PREFIX}-jbigkit \
           ${PKG_PREFIX}-lerc \
           ${PKG_PREFIX}-libc++ \
           ${PKG_PREFIX}-libdatrie \
           ${PKG_PREFIX}-libdeflate \
           ${PKG_PREFIX}-libepoxy \
           ${PKG_PREFIX}-libffi \
           ${PKG_PREFIX}-libiconv \
           ${PKG_PREFIX}-libidn2 \
           ${PKG_PREFIX}-libjpeg-turbo \
           ${PKG_PREFIX}-libnfs \
           ${PKG_PREFIX}-libpng \
           ${PKG_PREFIX}-libpsl \
           ${PKG_PREFIX}-libslirp \
           ${PKG_PREFIX}-libssh \
           ${PKG_PREFIX}-libssh2 \
           ${PKG_PREFIX}-libtasn1 \
           ${PKG_PREFIX}-libthai \
           ${PKG_PREFIX}-libtiff \
           ${PKG_PREFIX}-libunistring \
           ${PKG_PREFIX}-libunwind \
           ${PKG_PREFIX}-libusb \
           ${PKG_PREFIX}-libwebp \
           ${PKG_PREFIX}-libwinpthread-git \
           ${PKG_PREFIX}-lz4 \
           ${PKG_PREFIX}-lzo2 \
           ${PKG_PREFIX}-nettle \
           ${PKG_PREFIX}-openssl \
           ${PKG_PREFIX}-opus \
           ${PKG_PREFIX}-orc \
           ${PKG_PREFIX}-p11-kit \
           ${PKG_PREFIX}-pango \
           ${PKG_PREFIX}-pixman \
           ${PKG_PREFIX}-SDL2 \
           ${PKG_PREFIX}-SDL2_image \
           ${PKG_PREFIX}-snappy \
           ${PKG_PREFIX}-spice \
           ${PKG_PREFIX}-usbredir \
           ${PKG_PREFIX}-xz \
           ${PKG_PREFIX}-zlib \
           ${PKG_PREFIX}-zstd
mkdir -p "build-win" && cd "build-win"

echo "Configuring QEMU..."
../configure --disable-werror --enable-sdl --enable-gtk \
    --target-list="$TARGET_LIST" --enable-debug || exit 1

echo "Building QEMU..."
make -j$(nproc) || exit 1

# 准备部署工具
echo "Downloading ldd_deploy.sh..."
wget -q https://github.com/lostjared/ldd-deploy/raw/refs/heads/main/ldd_deploy.sh
chmod +x ldd_deploy.sh
cd ..
ROOT_DIR="$PWD"
DONE_DIR="$ROOT_DIR/done_zip"   # 输出目录
mkdir -p "$DONE_DIR"

cd build-win
ROOT_DIR="$PWD"
FIN_DIR="$ROOT_DIR/fin"

for target in "${TARGETS[@]}"; do
    arch=$(echo "$target" | cut -d'-' -f1)

    # 每次清空 fin 目录并建立统一的 bin/ 结构
    rm -rf "$FIN_DIR"
    mkdir -p "$FIN_DIR/bin/share"

    exe="qemu-system-${arch}.exe"
    if [[ ! -f "$exe" ]]; then
        echo "WARNING: $exe not found, skipping $target"
        continue
    fi

    # 部署可执行文件及其依赖库到 bin/ 目录
    ./ldd_deploy.sh -i "$exe" -o "$FIN_DIR/bin"
    cp "$exe" "$FIN_DIR/bin/"
    if [[ $target == "aarch64-softmmu" ]]; then
        mv "$FIN_DIR/bin/$exe" "$FIN_DIR/bin/qemu-system-arm64.exe"
    fi

    # 复制 BIOS 固件到 bin/share/
    cp -r ../pc-bios/* "$FIN_DIR/bin/share/"

    # 打包：只将 bin 目录压缩，保持根目录为 bin/
    echo "Packaging $target..."
    (cd "$FIN_DIR" && zip -r "$DONE_DIR/windows-${target}.zip" bin)
    echo "Created $DONE_DIR/windows-${target}.zip"
done

echo "All packages created in $DONE_DIR"