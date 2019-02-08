FROM ubuntu:bionic AS build

RUN apt-get update -qq \
  && DEBIAN_FRONTEND=noninteractive apt-get install -yqq \
  software-properties-common \
  && apt-get update -qq \
  && DEBIAN_FRONTEND=noninteractive apt-get install -yqq \
  clang-7           \
  cmake             \
  autoconf          \
  automake          \
  libtool           \
  git               \
  pkg-config        \
  unzip             \
  wget


WORKDIR /src

COPY scripts/build-deps.sh /src
RUN /src/build-deps.sh

COPY . /src

RUN mkdir build \
  && cd build \
  && cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-7 \
  -DCMAKE_CXX_COMPILER=clang++-7 \
  -DCMAKE_PREFIX_PATH=/opt/opencv;/opt/ffmpeg \
  && make -j5 \
  && make -j5 install

FROM ubuntu:bionic

COPY --from=build /opt/ffmpeg /opt/ffmpeg
COPY --from=build /opt/opencv /opt/opencv
COPY --from=build /opt/doge /opt/doge

ENTRYPOINT [ "/opt/doge/bin/doge-streamer" ]
