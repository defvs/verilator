#!/usr/bin/env python3
# DESCRIPTION: Verilator: Fault API generation and direct state mutation
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json

import vltest_bootstrap

test.scenarios('simulator')

test.compile(make_top_shell=False, make_main=False,
             verilator_flags2=["--exe", "--fault-api", "--fault-root", "t.u_dut",
                               "--fault-force",
                               test.pli_filename])

with open(test.obj_dir + "/fault_site_accounting.json", encoding="utf-8") as accounting_file:
    accounting = json.load(accounting_file)

summary = accounting["summary"]
assert accounting["schema_version"] == 1
assert accounting["manifest_type"] == "vfi_fault_site_accounting"
assert summary["invariant_holds"]
assert summary["candidate_bits"] == (
    summary["injectable_bits"] + summary["excluded_bits"] + summary["unresolved_bits"]
)
assert summary["candidate_objects"] == len(accounting["sites"])
assert summary["unresolved_bits"] == 0
sites = {site["path"]: site for site in accounting["sites"]}
assert sites["t.u_dut.source"]["disposition"] == "port_or_constant"
assert sites["t.u_dut.internal_d"]["disposition"] == "injectable"
assert sites["t.u_dut.state_q"]["disposition"] == "ambiguous_semantics"
assert sites["t.u_dut.state_q"]["generated_target_id"] >= 0

test.execute()

test.passes()
