# Runtime environment for the integration test: Electron 11 + pepper flash under Xvfb
# with a window manager, a JDK and FUSE 2 (optional at run time).
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        xvfb openbox x11-utils procps python3 openjdk-17-jdk \
        libgtk-3-0 libnss3 libasound2 libxss1 libgbm1 libx11-xcb1 libxtst6 \
        libatk-bridge2.0-0 libdrm2 libxshmfence1 libxkbfile1 libsecret-1-0 \
        fuse libfuse2 libxext6 libasan6 libubsan1 \
 && rm -rf /var/lib/apt/lists/*
