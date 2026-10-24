#!/bin/sh
set -eu

module_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../driver" && pwd)
module="$module_dir/ina219_demo.ko"

if [ ! -f "$module" ]; then
	printf 'Missing %s; build with make -C %s first.\n' "$module" "$module_dir" >&2
	exit 1
fi
sudo insmod "$module"
