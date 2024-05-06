/*
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2023 MonetDB B.V.
 */

#ifndef _OPT_ALIASES_
#define _OPT_ALIASES_
#include "opt_prelude.h"
#include "opt_support.h"
#include "mal_exception.h"

extern int OPTisAlias(InstrPtr p);
extern void OPTaliasRemap(InstrPtr p, int *alias);
extern str OPTaliasesImplementation(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr p);

#endif