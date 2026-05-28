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

# Standalone Redis provisioning CLI. It does NOT link libdbi: it compiles the
# vendored cJSON directly and speaks raw hiredis, so it builds in a
# libmongoc-free (redis-only) environment. Built only when hiredis is found.
open5gs_dbctl_redis_sources = files('''
    dbctl.c
    dbctl-redis.c
'''.split()) + files('../../lib/dbi/redis/cJSON.c')

executable('open5gs-dbctl-redis',
    sources : open5gs_dbctl_redis_sources,
    include_directories : [include_directories('.'),
                           include_directories('../../lib/dbi/redis'),
                           libinc],
    dependencies : [libcore_dep, libhiredis_dep],
    install_rpath : libdir,
    install : true)
