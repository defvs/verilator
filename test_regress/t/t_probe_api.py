#!/usr/bin/env python3
# DESCRIPTION: Verilator: Probe API golden capture and comparison
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('simulator')

probe_config = test.obj_dir + "/probe_config.txt"
with open(probe_config, "w", encoding="utf-8") as fh:
    fh.write("scalar t.u_dut.scalar all 4\n")
    fh.write("bus t.u_dut.data_bus all 4\n")
    fh.write("bit3 t.u_dut.data_bus[3] first 1\n")
    fh.write("elem t.u_dut.reg_bank[2] stop 1\n")
    fh.write("wide t.u_dut.wide_bus all 4\n")

test.compile(make_top_shell=False, make_main=False,
             verilator_flags2=["--exe", "--probe-config", probe_config, test.pli_filename])

test.file_grep(test.obj_dir + "/probe_points.json", '"name": "wide"')
test.file_grep_not(test.obj_dir + "/Vt_probe_api___024root.h", "unprobed")

test.execute(all_run_flags=["+golden=" + test.obj_dir + "/probe_api.golden"])

test.passes()
