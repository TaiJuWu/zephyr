#!/bin/bash
# run.sh [extra cbmc args]: prove the timeout list harness with CBMC
# (inside the zephyr-build image: apt-get install -y cbmc first)
Z=${ZEPHYR_BASE:-/workdir/zephyr}
cbmc "$(dirname "$0")/timeout_list_harness.c" -I "$Z" -I "$Z/include" \
  --bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check \
  --pointer-overflow-check --unwind ${UNWIND:-6} --unwinding-assertions "$@"
