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

    ogs-mongoc.h
    ogs-flatfile.h
    ogs-flatfile-state.h

    ogs-mongoc.c
    subscription.c
    session.c
    ims.c
    ogs-flatfile.c
    ogs-flatfile-state.c
'''.split())

# mongoless: libmongoc is no longer linked. The upstream sources above are
# still compiled as empty translation units (their bodies are gated by
# #if 0); replacement implementations live in ogs-flatfile{,-state}.c and
# pull in libyaml + hiredis.
libyaml_dep = dependency('yaml-0.1')
libhiredis_dep = dependency('hiredis')

libdbi_inc = include_directories('.')

libdbi = library('ogsdbi',
    sources : libdbi_sources,
    version : libogslib_version,
    c_args : '-DOGS_DBI_COMPILATION',
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep, libyaml_dep, libhiredis_dep],
    # mongoless: ogs-flatfile.c uses C99-style mid-block decls
    # (project-wide default is gnu89).
    override_options : ['c_std=gnu99'],
    install : true)

libdbi_dep = declare_dependency(
    link_with : libdbi,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep, libyaml_dep, libhiredis_dep])
