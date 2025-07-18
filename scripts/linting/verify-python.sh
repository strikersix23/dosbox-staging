#!/bin/bash

# SPDX-FileCopyrightText:  2019-2021 The DOSBox Staging Team
# SPDX-FileCopyrightText:  2019-2021 Patryk Obara <patryk.obara@gmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later

# $ ./verify-python.sh --disable=<msg_ids>
#
# This script uses exclusively python3 version of pylint; most distributions
# provide it in a package pylint, but some call it pylint3 or python3-pylint.

set -e

list_python_files () {
	git ls-files \
		| xargs file \
		| grep "Python script" \
		| cut -d ':' -f 1
}

main () {
	# Using "python3 -m pylint" to avoid using python2-only
	# version of pylint by mistake.
	python3 -m pylint --version >&2
	echo "Checking files:" >&2
	list_python_files >&2
	local -r rc="$(git rev-parse --show-toplevel)/.pylint"
	list_python_files | xargs -L 1000 python3 -m pylint --rcfile="$rc" "$@"
}

main "$@"
