#!/usr/bin/env bash
# install_deps.sh - Install build dependencies for the native Linux build of
# the xboxrecomp runtime libraries.
#
# Detects the host family from /etc/os-release and uses its package manager:
#   Debian/Ubuntu (incl. WSL) -> apt
#   Arch/CachyOS/Manjaro      -> pacman
#   Fedora/RHEL               -> dnf
#
#   bash tools/linux/install_deps.sh
#
# Requires sudo (will prompt for a password).
set -euo pipefail

# Overridable so the self-check below can feed it fake os-release files.
OS_RELEASE="${OS_RELEASE:-/etc/os-release}"

# Same dependencies throughout, spelled per distro (design principle A10:
# C, SDL3 and Vulkan only):
#   clang + cmake + ninja
#   Vulkan headers/loader + glslang - the nv2a_vk renderer
#   SDL3    - window, input, audio. Ubuntu 24.04 and Fedora 40 have no SDL3
#             package: build release-3.4.18 from source as the CI does
#             (.github/workflows/linux.yml) and pass its prefix to CMake.
detect_family() {
    local id id_like
    [ -r "$OS_RELEASE" ] || return 1
    id=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID:-}")
    id_like=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID_LIKE:-}")

    case " $id $id_like " in
        *" debian "*|*" ubuntu "*) echo debian ;;
        *" arch "*|*" archlinux "*) echo arch ;;
        *" fedora "*|*" rhel "*|*" centos "*) echo fedora ;;
        *) return 1 ;;
    esac
}

family=$(detect_family) || {
    echo "Could not identify the distro from $OS_RELEASE." >&2
    echo "Install these by hand: clang, cmake, ninja, the Vulkan headers and" >&2
    echo "loader, glslang, and SDL3 (all with headers)." >&2
    exit 1
}

case "$family" in
    debian)
        PKGS=(cmake ninja-build clang libvulkan-dev glslang-dev glslang-tools)
        echo "Detected Debian/Ubuntu. Installing ${#PKGS[@]} packages..."
        sudo apt-get update
        sudo apt-get install -y "${PKGS[@]}"
        ;;
    arch)
        PKGS=(base-devel clang cmake ninja vulkan-headers vulkan-icd-loader glslang sdl3)
        echo "Detected Arch. Installing ${#PKGS[@]} packages..."
        sudo pacman -S --needed "${PKGS[@]}"
        ;;
    fedora)
        PKGS=(clang cmake ninja-build vulkan-headers vulkan-loader-devel glslang-devel)
        echo "Detected Fedora/RHEL. Installing ${#PKGS[@]} packages..."
        sudo dnf install -y "${PKGS[@]}"
        ;;
esac

echo
echo "Done. Build deps installed."
