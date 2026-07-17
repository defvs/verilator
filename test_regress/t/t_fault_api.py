#!/usr/bin/env python3
# DESCRIPTION: Verilator: Fault API generation and direct state mutation
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('simulator')

test.compile(make_top_shell=False, make_main=False,
             verilator_flags2=["--exe", "--fault-api", "--fault-root", "t.u_dut",
                               "--fault-force",
                               test.pli_filename])

test.execute()

test.passes()
