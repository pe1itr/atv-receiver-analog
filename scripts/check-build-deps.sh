#!/bin/bash
# Read-only native Linux build checks. Never installs packages.
set -euo pipefail
ID=unknown
ID_LIKE=
if [[ -r /etc/os-release ]]; then . /etc/os-release; fi
case " $ID $ID_LIKE " in
    *debian*|*ubuntu*|*linuxmint*) family=apt ;;
    *fedora*|*rhel*) family=dnf ;;
    *) family=unknown ;;
esac
hint() {
    case "$family" in
        apt) printf '  sudo apt update\n  sudo apt install build-essential pkg-config libsdl2-dev libfftw3-dev libsoapysdr-dev libuhd-dev libpng-dev libfreetype6-dev libfontconfig1-dev fonts-dejavu-core\n' ;;
        dnf) printf '  sudo dnf install gcc make pkgconf-pkg-config sdl2-compat-devel fftw-devel SoapySDR-devel uhd-devel libpng-devel freetype-devel fontconfig-devel google-noto-sans-mono-vf-fonts\n' ;;
        *) echo 'Install a C11 compiler, make, pkg-config and development packages for SDL2, FFTW3 (float), SoapySDR, UHD, libpng, FreeType and Fontconfig.' ;;
    esac
    echo 'See INSTALL.md for distro-specific instructions and package/repository troubleshooting.'
}
read -r -a compiler <<< "${CC:-cc}"
pkg_config=${PKG_CONFIG:-pkg-config}
failed=0
for tool in "${compiler[0]}" make "$pkg_config"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required build tool not found: $tool" >&2
        failed=1
    fi
done
if ((failed)); then hint >&2; exit 1; fi
packages=(sdl2 fftw3f SoapySDR uhd libpng freetype2 fontconfig)
for package in "${packages[@]}"; do
    if "$pkg_config" --exists "$package"; then
        printf 'OK: %s %s\n' "$package" "$("$pkg_config" --modversion "$package")"
    else
        echo "ERROR: development dependency missing or unusable: $package" >&2
        "$pkg_config" --print-errors --exists "$package" >&2 || true
        failed=1
    fi
done
if ((failed)); then
    hint >&2
    echo 'If already installed in a custom prefix, set PKG_CONFIG_PATH to its lib/pkgconfig directory. Do not mix Windows and Linux .pc files.' >&2
    exit 1
fi
case "${HAVE_LIBIIO:-auto}" in
    0) echo 'INFO: PlutoSDR support explicitly disabled (HAVE_LIBIIO=0).' ;;
    auto|'')
        if "$pkg_config" --exists 'libiio >= 0.23' 'libiio < 1.0'; then
            echo "OK: PlutoSDR support (libiio $("$pkg_config" --modversion libiio))."
        else
            echo 'WARNING: building WITHOUT PlutoSDR: libiio >= 0.23 and < 1.0 is required.' >&2
            if "$pkg_config" --exists libiio; then
                echo "Found incompatible libiio $("$pkg_config" --modversion libiio); libiio 1.x is not supported." >&2
            fi
            case "$family" in
                apt) echo 'For PlutoSDR: sudo apt install libiio-dev' >&2 ;;
                dnf) echo 'For PlutoSDR: sudo dnf install libiio-devel' >&2 ;;
            esac
            echo 'Check pkg-config --modversion libiio, then rebuild with make -B all. For other SDRs you may continue.' >&2
        fi ;;
    1)
        if ! "$pkg_config" --exists 'libiio >= 0.23' 'libiio < 1.0'; then
            echo 'ERROR: HAVE_LIBIIO=1 requires libiio >= 0.23 and < 1.0. Install a compatible development package or use HAVE_LIBIIO=0.' >&2
            exit 1
        fi
        echo "OK: PlutoSDR support (libiio $("$pkg_config" --modversion libiio))." ;;
    *) echo 'ERROR: HAVE_LIBIIO must be 0 or 1 (or unset for automatic detection).' >&2; exit 1 ;;
esac
echo 'Build dependencies OK. Hardware drivers and USB permissions are configured separately; see INSTALL.md.'
