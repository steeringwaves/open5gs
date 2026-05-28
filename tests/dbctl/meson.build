# Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>

# This file is part of Open5GS.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Unit tests for the open5gs-dbctl-redis pure functions. They compile the
# CLI's pure source (dbctl-subscriber.c) together with the vendored cJSON, so
# the suite needs neither libdbi nor a live Redis. Built only when hiredis is
# found, matching the tool's build gate.
testdbctl_sources = files('''
    abts-main.c
    dbctl-test.c
'''.split()) + files('../../src/dbctl/dbctl-subscriber.c') + \
    files('../../lib/dbi/redis/cJSON.c')

testdbctl_exe = executable('dbctl-tests',
    sources : testdbctl_sources,
    c_args : testunit_core_cc_flags,
    include_directories : [include_directories('../../src/dbctl'),
                           include_directories('../../lib/dbi/redis')],
    dependencies : [libcore_dep])

test('dbctl', testdbctl_exe, is_parallel : false, suite: 'dbctl')
