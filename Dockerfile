FROM ubuntu:24.04

ARG BINUTILS_VERSION=2.43
ARG GCC_VERSION=14.2.0
ARG TARGET=x86_64-elf
ARG PREFIX=/opt/cross
ARG JOBS=4

ENV PATH="${PREFIX}/bin:${PATH}"

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    bison \
    flex \
    libgmp-dev \
    libmpc-dev \
    libmpfr-dev \
    texinfo \
    wget \
    ca-certificates \
    nasm \
    xorriso \
    grub-pc-bin \
    grub-common \
    mtools \
    dosfstools \
    qemu-system-x86 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/build

# Download sources
RUN wget -q https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz \
    && wget -q https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz \
    && tar xf binutils-${BINUTILS_VERSION}.tar.xz \
    && tar xf gcc-${GCC_VERSION}.tar.xz

# Build binutils
RUN mkdir build-binutils && cd build-binutils \
    && ../binutils-${BINUTILS_VERSION}/configure \
        --target=${TARGET} \
        --prefix=${PREFIX} \
        --with-sysroot \
        --disable-nls \
        --disable-werror \
    && make -j${JOBS} \
    && make install

# Build GCC (freestanding cross-compiler, no hosted libs)
RUN mkdir build-gcc && cd build-gcc \
    && ../gcc-${GCC_VERSION}/configure \
        --target=${TARGET} \
        --prefix=${PREFIX} \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
    && make -j${JOBS} all-gcc \
    && make -j${JOBS} all-target-libgcc \
    && make install-gcc \
    && make install-target-libgcc

# Clean up build artifacts to shrink the image
RUN rm -rf /tmp/build

WORKDIR /src
