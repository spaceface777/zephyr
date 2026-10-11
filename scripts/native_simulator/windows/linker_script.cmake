# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

file(READ "${NSI_BASE}" base)
file(READ "${NSI_LOCALIZED}" localized)
file(WRITE "${NSI_OUTPUT}" "${base}${localized}")
