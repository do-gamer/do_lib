# Toolchain for portable Linux builds of DarkTanos.so / libdo_lib.so.
# Built against glibc 2.31 (Ubuntu 20.04), so the libraries load on Ubuntu 20.04+,
# Linux Mint 20+, Debian 11+, Fedora 32+, and other current distributions.
# libstdc++/libgcc are linked statically (see CMakeLists.txt).
FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates cmake make g++ binutils \
        libx11-dev libxext-dev openjdk-11-jdk-headless \
 && rm -rf /var/lib/apt/lists/*
