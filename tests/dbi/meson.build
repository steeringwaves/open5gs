# Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

testdbi_sources = files('''
    abts-main.c
    change-event-test.c
'''.split())

testdbi_exe = executable('dbi-tests',
    sources : testdbi_sources,
    c_args : testunit_core_cc_flags,
    dependencies : [libdbi_dep, libcore_dep])

test('dbi', testdbi_exe, is_parallel : false, suite: 'dbi')
