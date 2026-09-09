#!/bin/bash
#
# Remove script for sysrepo-gnxi server YANG modules.
#
# Uninstall the YANG modules installed by setup.sh from sysrepo. Removal is
# best-effort: failures are ignored so that partially installed setups and
# modules shared with other consumers (which may still be in use) do not
# abort the run.
#
# The following environment variable is optional:
#
#   SYSREPOCTL_EXECUTABLE   path to the sysrepoctl binary
#                           (defaults to sysrepoctl from PATH)

set -euo pipefail

SYSREPOCTL="${SYSREPOCTL_EXECUTABLE:-sysrepoctl}"

echo "Removing sysrepo-gnxi YANG modules from sysrepo..."

# uninstall the modules in reverse dependency order
for mod in sysrepo-gnxi-server sysrepo-gnxi-users \
        ietf-tls-server ietf-tls-common ietf-truststore ietf-keystore \
        ietf-crypto-types iana-tls-cipher-suite-algs iana-crypt-hash; do
    echo -n "  $mod..."
    if ! "$SYSREPOCTL" -u "$mod" >/dev/null 2>&1; then
        echo " FAILED"
    else
        echo " ok"
    fi
done
