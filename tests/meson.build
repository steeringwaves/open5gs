# Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>

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

testinc = include_directories('.')

subdir('core')
subdir('crypt')
subdir('sctp')
subdir('unit')
subdir('dbi')
subdir('af')

# The integration tests below provision subscribers into MongoDB via BSON
# (tests/common, and the libtestepc/libtest5gc aggregates in tests/app).
# They are built only when the MongoDB backend is enabled; a Redis-only
# build (-Dmongo=disabled) skips them so the tree compiles without
# libmongoc/libbson headers present.
if libmongoc_dep.found()
    subdir('common')
    subdir('app')
    subdir('registration')
    subdir('vonr')
    subdir('slice')
    subdir('attach')
    subdir('volte')
    subdir('csfb')
    subdir('310014')
    subdir('handover')
    subdir('non3gpp')
    subdir('transfer')
endif
