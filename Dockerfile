FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf \
    automake autoconf libtool \
    git wget curl \
    pkg-config \
    python2.7-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# sigc++ 1.2 für ARM cross-compile bauen
RUN wget -q http://ftp.gnome.org/pub/GNOME/sources/libsigc++/1.2/libsigc++-1.2.7.tar.bz2 && \
    tar xjf libsigc++-1.2.7.tar.bz2

RUN cd libsigc++-1.2.7 && \
    ./configure \
        --host=arm-linux-gnueabihf \
        --prefix=/build/sysroot \
        CC=arm-linux-gnueabihf-gcc \
        CXX=arm-linux-gnueabihf-g++ && \
    make -j$(nproc) && \
    make install

# Python 2.7 ARM headers (Ubuntu 18.04 hat nur x86 python2.7-dev)
# Wir holen die armhf version
RUN dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y python2.7-dev:armhf libssl-dev:armhf libuchardet-dev:armhf || true

# Alternativer Python 2.7 Header-Ansatz: x86 Include + ARM pyconfig
COPY sysroot/usr/include/python2.7/ /build/sysroot/usr/include/python2.7/

# OpenSSL Headers (für ARM)
RUN apt-get install -y libssl-dev 2>/dev/null || true

COPY build-serviceapp.sh /build/
RUN chmod +x /build/build-serviceapp.sh

CMD ["/build/build-serviceapp.sh"]
