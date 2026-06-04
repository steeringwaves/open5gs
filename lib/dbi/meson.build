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

mongoless = get_option('mongoless')

# Upstream sources are always in the source list. Their bodies are gated
# by #ifndef MONGOLESS so they compile to empty TUs when mongoless is on.
libdbi_sources = files('''
    ogs-dbi.h

    ogs-mongoc.h

    ogs-mongoc.c
    subscription.c
    session.c
    ims.c
'''.split())

libdbi_extra_args = []
libdbi_extra_deps = []
libdbi_override_options = []

if mongoless
    libdbi_sources += files('''
        ogs-flatfile.h
        ogs-flatfile-state.h
        ogs-flatfile-watcher.h

        ogs-flatfile.c
        ogs-flatfile-state.c
        ogs-flatfile-watcher.c
    '''.split())
    libdbi_extra_args += ['-DMONGOLESS']
    libdbi_extra_deps += [
        dependency('yaml-0.1'),
        dependency('hiredis'),
    ]
    # ogs-flatfile.c uses C99-style mid-block decls; project-wide
    # default is gnu89. Only override when those sources are included.
    libdbi_override_options += ['c_std=gnu99']
else
    libdbi_extra_deps += [dependency('libmongoc-1.0')]
endif

libdbi_inc = include_directories('.')

libdbi = library('ogsdbi',
    sources : libdbi_sources,
    version : libogslib_version,
    c_args : ['-DOGS_DBI_COMPILATION'] + libdbi_extra_args,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep] + libdbi_extra_deps,
    override_options : libdbi_override_options,
    install : true)

libdbi_dep = declare_dependency(
    link_with : libdbi,
    include_directories : [libdbi_inc, libinc],
    dependencies : [libproto_dep] + libdbi_extra_deps,
    # Consumers (HSS, UDR, PCRF, PCF, tests/common) include ogs-dbi.h
    # whose preprocessor branches depend on MONGOLESS — propagate it.
    compile_args : libdbi_extra_args)
