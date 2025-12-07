#!/bin/bash
# Build and test script for Kodi
# This script installs dependencies, builds Kodi, and runs tests

set -e  # Exit on first error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "=== Kodi Build and Test Script ==="
echo "Source directory: ${SCRIPT_DIR}"
echo "Build directory: ${BUILD_DIR}"

# Function to install dependencies
install_dependencies() {
    echo ""
    echo "=== Installing dependencies ==="

    # Core build tools (most already installed)
    PACKAGES="
        autoconf automake autopoint gettext autotools-dev cmake curl
        gawk gcc g++ cpp gperf libtool meson nasm ninja-build
        python3-dev python3-pil swig unzip zip
    "

    # Required libraries
    PACKAGES="${PACKAGES}
        libasound2-dev libass-dev libavahi-client-dev libavahi-common-dev
        libbluetooth-dev libbluray-dev libbz2-dev libcdio-dev
        libcurl4-openssl-dev libdbus-1-dev libegl1-mesa-dev
        libenca-dev libflac-dev libfontconfig-dev libfmt-dev
        libfreetype6-dev libfribidi-dev libfstrcmp-dev libgcrypt-dev
        libgif-dev libgl1-mesa-dev libglew-dev libglu1-mesa-dev
        libgnutls28-dev libgpg-error-dev libgtest-dev libiso9660-dev
        libjpeg-dev liblcms2-dev libltdl-dev liblzo2-dev
        libmicrohttpd-dev libnfs-dev libogg-dev libpcre3-dev
        libplist-dev libpng-dev libpulse-dev libshairplay-dev
        libsmbclient-dev libspdlog-dev libsqlite3-dev libssl-dev
        libtag1-dev libtiff-dev libtinyxml-dev libtinyxml2-dev
        libudev-dev libunistring-dev libva-dev libvdpau-dev
        libvorbis-dev libxkbcommon-dev libxmu-dev libxrandr-dev
        libxslt1-dev libxt-dev lsb-release uuid-dev zlib1g-dev
        flatbuffers-compiler libflatbuffers-dev rapidjson-dev
    "

    # Try to install libcec if available
    PACKAGES="${PACKAGES} libcec-dev libp8-platform-dev"

    # Try to install crossguid if available
    PACKAGES="${PACKAGES} libcrossguid-dev"

    # Wayland packages (optional)
    PACKAGES="${PACKAGES} waylandpp-dev wayland-protocols libwayland-dev"

    echo "Installing packages..."
    sudo apt-get update
    sudo apt-get install -y ${PACKAGES} 2>/dev/null || {
        echo "Some packages may not be available, continuing with available ones..."
        for pkg in ${PACKAGES}; do
            sudo apt-get install -y ${pkg} 2>/dev/null || echo "Package ${pkg} not available, skipping..."
        done
    }

    echo "Dependencies installation complete."
}

# Function to configure build
configure_build() {
    echo ""
    echo "=== Configuring build ==="

    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    # Configure with testing enabled, using internal dependencies where needed
    cmake "${SCRIPT_DIR}" \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCORE_PLATFORM_NAME=x11 \
        -DAPP_RENDER_SYSTEM=gl \
        -DENABLE_TESTING=ON \
        -DENABLE_INTERNAL_GTEST=ON \
        -DENABLE_INTERNAL_CROSSGUID=ON \
        -DENABLE_INTERNAL_FLATBUFFERS=ON \
        -DENABLE_INTERNAL_FMT=ON \
        -DENABLE_INTERNAL_SPDLOG=ON \
        -DENABLE_INTERNAL_RapidJSON=ON \
        -DENABLE_INTERNAL_FSTRCMP=ON \
        -DENABLE_UPNP=ON \
        -DENABLE_PYTHON=ON \
        -DCMAKE_BUILD_TYPE=Debug

    echo "Configuration complete."
}

# Function to build project
build_project() {
    echo ""
    echo "=== Building project ==="

    cd "${BUILD_DIR}"

    # Get number of processors for parallel build
    NPROCS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

    cmake --build . -- -j${NPROCS}

    echo "Build complete."
}

# Function to build and run tests
run_tests() {
    echo ""
    echo "=== Building and running tests ==="

    cd "${BUILD_DIR}"

    # Build the test target
    make kodi-test -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

    echo ""
    echo "=== Running tests ==="

    # Run the tests
    ./kodi-test --gtest_output=xml:test_results.xml

    echo ""
    echo "Tests complete. Results saved to ${BUILD_DIR}/test_results.xml"
}

# Main execution
case "${1:-all}" in
    deps)
        install_dependencies
        ;;
    configure)
        configure_build
        ;;
    build)
        build_project
        ;;
    test)
        run_tests
        ;;
    all)
        install_dependencies
        configure_build
        build_project
        run_tests
        ;;
    *)
        echo "Usage: $0 {deps|configure|build|test|all}"
        echo "  deps      - Install dependencies"
        echo "  configure - Configure CMake build"
        echo "  build     - Build the project"
        echo "  test      - Build and run tests"
        echo "  all       - Run all steps (default)"
        exit 1
        ;;
esac

echo ""
echo "=== Done ==="
