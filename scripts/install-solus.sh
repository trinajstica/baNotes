#!/usr/bin/env bash
set -euo pipefail

# Installer script for Solus (eopkg).
# - checks for basic build tools and pkg-config modules used by this project
# - attempts to install candidate packages via `sudo eopkg install`
# Note: Solus package names vary; this script tries common candidates and
# reports any modules that still cannot be satisfied so the user can search
# the repository for the correct package names.

echo "Checking environment for baNotes (Solus)..."

if ! command -v eopkg >/dev/null 2>&1; then
    echo "Error: this script expects Solus package manager 'eopkg' to be available." >&2
    echo "Please run on Solus or install packages manually." >&2
    exit 1
fi

MISSING_PACKAGES=()

# Build tools we expect
BUILD_TOOLS=(gcc make pkg-config)
for t in "${BUILD_TOOLS[@]}"; do
    if ! command -v "$t" >/dev/null 2>&1; then
        MISSING_PACKAGES+=("$t")
    fi
done

# Libraries required (pkg-config module names)
REQUIRED_PKG_MODULES=("gtk+-3.0" "ayatana-appindicator3-0.1")

# Map pkg-config modules to candidate Solus package names (order: preferred -> fallback)
# Prefer '-devel' packages which provide headers and .pc files for development
declare -A CANDIDATES
CANDIDATES["gtk+-3.0"]="libgtk-3-devel libgtk-3 gtk+3 gtk3 gtk+-3"
CANDIDATES["ayatana-appindicator3-0.1"]="libayatana-appindicator-devel libayatana-appindicator ayatana-appindicator3"

PKGS_TO_INSTALL=()

echo "Checking pkg-config modules..."
for mod in "${REQUIRED_PKG_MODULES[@]}"; do
    if pkg-config --exists "$mod"; then
        echo "  OK: pkg-config can find $mod"
        continue
    fi
    echo "  Missing pkg-config module: $mod"
    # add candidate package names for installation attempt
    for cand in ${CANDIDATES[$mod]}; do
        PKGS_TO_INSTALL+=("$cand")
    done
done

# Add build tools to install list (package names typically same as command)
for cmd in "${MISSING_PACKAGES[@]:-}"; do
    PKGS_TO_INSTALL+=("$cmd")
done

echo
# filter out empty entries and duplicates from PKGS_TO_INSTALL
declare -A _seen=()
FILTERED=()
for p in "${PKGS_TO_INSTALL[@]}"; do
    if [ -z "$p" ]; then
        continue
    fi
    if [ -n "${_seen[$p]:-}" ]; then
        continue
    fi
    _seen[$p]=1
    FILTERED+=("$p")
done

if [ ${#FILTERED[@]} -eq 0 ]; then
    echo "All required tools and modules appear present. Nothing to install."
    exit 0
fi

echo "The script will attempt to install the following candidate packages via eopkg:"
for p in "${FILTERED[@]}"; do
    echo "  - $p"
done

read -r -p "Run 'sudo eopkg install -y' for these packages now? [Y/n] " resp
resp=${resp:-Y}
if [[ ! "$resp" =~ ^([yY]|[yY][eE][sS])$ ]]; then
    echo "Aborted by user. Please install the packages manually and re-run this script.";
    exit 1
fi

FAILED=()
for pkg in "${FILTERED[@]}"; do
    echo "Installing $pkg..."
    if ! sudo eopkg install -y "$pkg" >/dev/null 2>&1; then
        echo "  Failed to install $pkg (it may not exist under that name)."
        FAILED+=("$pkg")
    else
        echo "  Installed $pkg"
    fi
done

echo
echo "Re-checking pkg-config modules and tools..."
for mod in "${REQUIRED_PKG_MODULES[@]}"; do
    if pkg-config --exists "$mod"; then
        echo "  OK: $mod"
    else
        echo "  STILL MISSING: $mod"
    fi
done

if [ ${#FAILED[@]} -ne 0 ]; then
    echo
    echo "Some candidate packages failed to install:" >&2
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    echo
    echo "On Solus you can try to find the correct package name with:" >&2
    echo "  eopkg search <keyword>" >&2
    echo "Example: 'eopkg search appindicator' or 'eopkg search gtk'" >&2
fi

echo
echo "If any pkg-config modules are still missing, locate the Solus package that provides them and install it."
echo "Afterwards you should be able to build with: 'make' and install with 'sudo make install'"

exit 0
