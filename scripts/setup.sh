#!/bin/bash
#
# Setup script for sysrepo-gnxi server YANG modules.
#
# Install the YANG modules required by the gNMI server into sysrepo,
# enable the required features and set filesystem permissions on the
# private modules.
#
# The following environment variables must be defined when executing
# this script:
#
#   GNXI_MODULE_DIR         directory with the YANG module files
#   SYSREPOCTL_EXECUTABLE   path to the sysrepoctl binary
#
# The following environment variables are optional and enable the minimal
# configuration import (so that the server is runnable right after the
# installation):
#
#   GNXI_CONFIG_FILE        path to the minimal configuration JSON file
#   SYSREPOCFG_EXECUTABLE   path to the sysrepocfg binary
#
# If the install is staged into a temporary location by the packaging
# tooling CMake exports DESTDIR and the script prepends it to the paths.

set -euo pipefail

if [ -z "${GNXI_MODULE_DIR:-}" ] || [ -z "${SYSREPOCTL_EXECUTABLE:-}" ]; then
    echo "Required environment variables GNXI_MODULE_DIR and SYSREPOCTL_EXECUTABLE not defined!"
    exit 1
fi

MODDIR="${DESTDIR:-}${GNXI_MODULE_DIR}"
SYSREPOCTL="$SYSREPOCTL_EXECUTABLE"
if [ ! -d "$MODDIR" ]; then
    echo "$0: YANG module directory not found: $MODDIR" >&2
    exit 1
fi

# install a module into sysrepo, features to enable are passed as extra arguments
install_module() {
    local file=$1
    shift

    echo -n "  $file..."
    if ! "$SYSREPOCTL" -i "$MODDIR/$file" -s "$MODDIR" "$@" >/dev/null 2>&1; then
        echo " FAILED"
        "$SYSREPOCTL" -i "$MODDIR/$file" -s "$MODDIR" "$@" || exit 1
    fi
    echo " ok"
}

echo "Installing YANG modules..."

install_module "iana-tls-cipher-suite-algs@2024-03-16.yang"
install_module "ietf-crypto-types@2024-10-10.yang" \
    -e cleartext-private-keys \
    -e one-asymmetric-key-format
install_module "ietf-keystore@2024-10-10.yang" \
    -e central-keystore-supported \
    -e asymmetric-keys
install_module "ietf-truststore@2024-10-10.yang" \
    -e central-truststore-supported \
    -e certificates
install_module "ietf-tls-common@2024-10-10.yang"
install_module "ietf-tls-server@2024-10-10.yang" \
    -e server-ident-x509-cert \
    -e client-auth-supported \
    -e client-auth-x509-cert
install_module "sysrepo-gnxi-server.yang"
install_module "sysrepo-gnxi-users.yang"

echo "Setting permissions on private modules..."

# private modules whose data files must be readable/writable only by their owner
for mod in sysrepo-gnxi-server sysrepo-gnxi-users; do
    "$SYSREPOCTL" -c "$mod" -p 600 >/dev/null 2>&1 || true
done

# import the minimal configuration so that the server is runnable right
# after the installation, but only when it has not been configured yet
# (an upgrade must not overwrite the operator's configuration)
if [ -n "${GNXI_CONFIG_FILE:-}" ] && [ -n "${SYSREPOCFG_EXECUTABLE:-}" ]; then
    CFGFILE="${DESTDIR:-}${GNXI_CONFIG_FILE}"

    if [ ! -f "$CFGFILE" ]; then
        echo "$0: minimal configuration not found: $CFGFILE" >&2
        exit 1
    fi

    # the server must have at least one listen endpoint to be runnable
    if "$SYSREPOCFG_EXECUTABLE" -G /sysrepo-gnxi-server:server/listen/endpoint/name \
        >/dev/null 2>&1; then
        echo "Server already configured, skipping the minimal configuration."
    else
        echo "Importing the minimal configuration..."
        "$SYSREPOCFG_EXECUTABLE" --edit="$CFGFILE" --format json --datastore running
    fi
fi

echo "YANG module setup complete."
