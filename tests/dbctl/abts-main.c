/*
 * Copyright (C) 2026 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ogs-core.h"
/*
 * The Docker-gated integration case in dbctl-test.c includes ogs-dbi.h, which
 * redefines OGS_LOG_DOMAIN to the dbi library's __ogs_dbi_domain. Its ogs_info()/
 * ogs_error() (and those inside ogs_dbi_*) therefore log under that domain.
 * ogs_log_vprintf() aborts if a domain id has no installed domain, so install
 * __ogs_dbi_domain here up front (mirrors tests/dbi/abts-main.c). The pure-
 * function unit tests do not log, so this is harmless for the no-Redis path.
 */
#include "ogs-dbi.h"
#include "core/abts.h"

abts_suite *test_dbctl_subscriber(abts_suite *suite);

const struct testlist {
    abts_suite *(*func)(abts_suite *suite);
} alltests[] = {
    { test_dbctl_subscriber },
    { NULL },
};

int main(int argc, const char *const *argv)
{
    int i;
    abts_suite *suite = NULL;

    ogs_core_initialize();
    atexit(ogs_core_terminate);

    ogs_log_install_domain(&__ogs_dbi_domain, "dbi", OGS_LOG_INFO);

    for (i = 0; alltests[i].func; i++)
        suite = alltests[i].func(suite);

    return abts_report(suite);
}
