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

# The change-event and backend-registry suites are backend-agnostic and always
# built. The redis-* suites include <hiredis.h> via the redis backend header, so
# they are only built when the Redis backend is enabled (else a -Dredis=disabled
# build has no hiredis on the include path).
testdbi_sources = files('''
    abts-main.c
    change-event-test.c
    backend-registry-test.c
'''.split())

testdbi_c_args = testunit_core_cc_flags

if libhiredis_dep.found()
    testdbi_sources += files('''
        redis-parse-test.c
        redis-equivalence-test.c
    '''.split())
    testdbi_c_args += '-DHAVE_REDIS_TESTS'
endif

testdbi_exe = executable('dbi-tests',
    sources : testdbi_sources,
    c_args : testdbi_c_args,
    dependencies : [libdbi_dep, libcore_dep])

test('dbi', testdbi_exe, is_parallel : false, suite: 'dbi')
