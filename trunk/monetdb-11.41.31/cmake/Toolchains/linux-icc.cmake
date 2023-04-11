#[[
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0.  If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Copyright 1997 - July 2008 CWI, August 2008 - 2021 MonetDB B.V.
#]]

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_COMPILER_VENDOR "intel")

set(CMAKE_C_COMPILER icc)
set(CMAKE_C_FLAGS "-O0 " CACHE STRING "" FORCE)
