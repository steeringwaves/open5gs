# Copyright (C) 2019-2023 by Sukchan Lee <acetcom@gmail.com>

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

libdbi_sources = files('''
    ogs-dbi.h
    ogs-dbi-backend.h

    ogs-mongoc.h

    ogs-dbi-core.c
    subscription.c
    session.c
    ims.c
'''.split())

backend_deps = []
backend_c_args = []

libmongoc_dep = dependency('libmongoc-1.0', required: get_option('mongo'))
if libmongoc_dep.found()
    libdbi_sources += files('''
        mongoc/mongoc-internal.h
        mongoc/mongoc-backend.c
        mongoc/mongoc-subscription.c
        mongoc/mongoc-session.c
        mongoc/mongoc-ims.c
        mongoc/mongoc-watch.c
    '''.split())
    backend_deps += libmongoc_dep
    backend_c_args += '-DOGS_DBI_HAVE_MONGOC'
endif

libhiredis_dep = dependency('hiredis', required: get_option('redis'))
if libhiredis_dep.found()
    backend_deps += libhiredis_dep
    backend_c_args += '-DOGS_DBI_HAVE_REDIS'
endif

if not libmongoc_dep.found() and not libhiredis_dep.found()
    error('At least one of -Dmongo / -Dredis must be enabled')
endif

libdbi_inc = include_directories('.', 'mongoc')

libdbi = library('ogsdbi',
    sources : libdbi_sources,
    version : libogslib_version,
    c_args : ['-DOGS_DBI_COMPILATION'] + backend_c_args,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep] + backend_deps,
    install : true)

libdbi_dep = declare_dependency(
    link_with : libdbi,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep] + backend_deps)
