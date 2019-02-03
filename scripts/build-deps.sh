#!/bin/bash

set -e
set -x

FFMPEG_DIR=/opt/ffmpeg
OPENCV_DIR=/opt/opencv

export CC=clang-7
export CXX=clang++-7
export PATH=$FFMPEG_DIR/bin:$OPENCV_DIR/bin:$PATH
export PKG_CONFIG_PATH=$FFMPEG_DIR/lib/pkgconfig:$OPENCV_DIR/lib/pkgconfig
export LD_LIBRARY_PATH=$FFMPEG_DIR/lib:$OPENCV_DIR/lib

alias make="make -j5"

# Temporary directory stuff
if [ "$1" != "" ]; then
    TMP_DIR=$1
    echo "Using TMP_DIR=$TMP_DIR"
    mkdir -p $TMP_DIR
else
    TMP_DIR=$(mktemp -d -t tmp.XXXXXXXXXX)
    WIPE_TMP="yes"
fi

trap cleanup EXIT

function cleanup {
    if [ "$WIPE_TMP" == "yes" ]; then
        echo "Cleaning up"
        rm -rf $TMP_DIR
    fi
}

mkdir -p $FFMPEG_DIR
mkdir -p $OPENCV_DIR
pushd $TMP_DIR
mkdir -p ffmpeg_sources

pushd ffmpeg_sources

# Build nasm
wget -q -c https://www.nasm.us/pub/nasm/releasebuilds/2.13.03/nasm-2.13.03.tar.bz2
tar xjf nasm-2.13.03.tar.bz2
pushd nasm-2.13.03
./autogen.sh
./configure --prefix=$FFMPEG_DIR --bindir=$FFMPEG_DIR/bin
make
make install
popd

# Build yasm
wget -q -c -O yasm-1.3.0.tar.gz https://www.tortall.net/projects/yasm/releases/yasm-1.3.0.tar.gz
tar xzf yasm-1.3.0.tar.gz
pushd yasm-1.3.0
./configure --prefix=$FFMPEG_DIR --bindir=$FFMPEG_DIR/bin
make
make install
popd

# Build x264
git -C x264 pull 2> /dev/null || git clone --depth 1 https://git.videolan.org/git/x264
pushd x264
./configure --prefix=$FFMPEG_DIR --bindir=$FFMPEG_DIR/bin --enable-static --enable-pic
make
make install
popd

# Build fdk-aac
git -C fdk-aac pull 2> /dev/null || git clone --depth 1 https://github.com/mstorsjo/fdk-aac
pushd fdk-aac
autoreconf -fiv
./configure --prefix=$FFMPEG_DIR --disable-shared
make
make install
popd

# Build FFmpeg
wget -q -c https://ffmpeg.org/releases/ffmpeg-4.1.tar.xz
tar xf ffmpeg-4.1.tar.xz
pushd ffmpeg-4.1

./configure \
--prefix=$FFMPEG_DIR \
--pkg-config-flags="--static" \
--extra-cflags="-I$FFMPEG_DIR/include" \
--extra-ldflags="-L$FFMPEG_DIR/lib"  \
--extra-libs="-lpthread -lm" \
--bindir=$FFMPEG_DIR/bin  \
--enable-gpl \
--enable-libfdk-aac \
--enable-libx264 \
--enable-nonfree \
--enable-pthreads \
--disable-doc \
--cc=$CC \
--cxx=$CXX

make
make install
hash -r
popd
popd

# Build OpenCV
wget -q -c https://github.com/opencv/opencv/archive/4.0.1.zip
unzip -oq 4.0.1.zip
pushd opencv-4.0.1
mkdir -p build
pushd build

cmake .. \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_INSTALL_PREFIX=$OPENCV_DIR \
-DCMAKE_EXE_LINKER_FLAGS=-L$FFMPEG_DIR/lib \
-DCMAKE_CXX_COMPILER=$CXX \
-DCMAKE_C_COMPILER=$CC \
-DBUILD_SHARED_LIBS=OFF \
-DBUILD_EXAMPLES=OFF \
-DBUILD_TESTS=OFF \
-DBUILD_DOCS=OFF \
-DOPENCV_ENABLE_NONFREE=ON \
-DOPENCV_GENERATE_PKGCONFIG=ON \
-DBUILD_PERF_TESTS=OFF

make
make install

echo "Success"
