#!/usr/bin/env bash
set -eo pipefail
EDK2_ROOT=${EDK2_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
cd "$EDK2_ROOT"
export WORKSPACE="$EDK2_ROOT"
export PACKAGES_PATH="$EDK2_ROOT"
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}
export EDK_TOOLS_PATH="$EDK2_ROOT/BaseTools"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
source edksetup.sh --reconfig
build -a AARCH64 -t CLANGDWARF -b "${EDK2_TARGET:-DEBUG}" -p Platform/OrangePi/OrangePi4ProPkg/OrangePi4Pro.dsc -n "${JOBS:-2}"
