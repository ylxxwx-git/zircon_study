# Dockerfile for an ARM Bare-Metal Cross-Compilation Environment

# Stage 1: Define the base environment
# We use Debian Bullseye (or a similar lightweight distribution) as the base
FROM debian:bullseye-slim

# Set environment variables to avoid interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Update package list and install necessary dependencies for development and toolchain
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        git \
        wget \
        ca-certificates \
        libncurses5 \
        python3 \
        unzip \
        bison \
        flex \
        libgmp3-dev \
        libmpfr-dev \
        libisl-dev \
        libcloog-isl-dev \
        libelf-dev \
        texinfo \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# --- Install the ARM Bare-Metal Toolchain (arm-none-eabi) ---
# We will download the official GCC ARM Embedded Toolchain from ARM's website.
# NOTE: Replace the URL and filename with the latest version if needed.
ENV TOOLCHAIN_VERSION 12.3.rel1
ENV TOOLCHAIN_FILE gcc-arm-12.3.rel1-2023.05-x86_64-arm-none-eabi.tar.xz
ENV TOOLCHAIN_URL https://developer.arm.com/-/media/Files/downloads/gnu/12.3.rel1/binrel/${TOOLCHAIN_FILE}
ENV TOOLCHAIN_DIR /opt/arm-toolchain

# Download, extract, and clean up the toolchain
RUN mkdir -p ${TOOLCHAIN_DIR} && \
    wget -q ${TOOLCHAIN_URL} -O /tmp/${TOOLCHAIN_FILE} && \
    tar xJf /tmp/${TOOLCHAIN_FILE} -C ${TOOLCHAIN_DIR} --strip-components=1 && \
    rm /tmp/${TOOLCHAIN_FILE}

# Add the toolchain bin directory to the PATH
ENV PATH="${PATH}:${TOOLCHAIN_DIR}/bin"

# Set the working directory for the OS source code, which will be mounted by the user
WORKDIR /usr/src/os

# Command to provide a convenient shell if running interactively
CMD ["/bin/bash"]

# Final environment check (Optional, for debugging)
# RUN arm-none-eabi-gcc --version
