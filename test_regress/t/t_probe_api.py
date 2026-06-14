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
    fh.write('create_probe "scalar" --path t.u_dut.scalar --mode all --log-limit 4\n')
    fh.write('create_probe "bus" --path t.u_dut.data_bus --mode all --log-limit 4\n')
    fh.write('create_probe "bit3" --path t.u_dut.data_bus[3]\n')
    fh.write('create_probe "elem" --path t.u_dut.reg_bank[2]\n')
    fh.write('create_probe "wide" --path t.u_dut.wide_bus --mode all --log-limit 4\n')
    fh.write('create_checker "error" --path t.u_dut.error_flag\n')
    fh.write('create_assessment "integrity" --probe elem --checker error '
             '--max-delay 10ns\n')

test.compile(make_top_shell=False, make_main=False,
             verilator_flags2=["--exe", "--probe-config", probe_config, test.pli_filename])

test.file_grep(test.obj_dir + "/probe_points.json", '"name": "wide"')
test.file_grep_not(test.obj_dir + "/Vt_probe_api___024root.h", "unprobed")

test.execute(all_run_flags=["+golden=" + test.obj_dir + "/probe_api.golden"])

test.passes()
