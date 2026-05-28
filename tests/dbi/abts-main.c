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

#include "ogs-dbi.h"
#include "core/abts.h"

abts_suite *test_change_event(abts_suite *suite);
abts_suite *test_backend_registry(abts_suite *suite);
abts_suite *test_redis_parse(abts_suite *suite);
abts_suite *test_redis_equivalence(abts_suite *suite);

const struct testlist {
    abts_suite *(*func)(abts_suite *suite);
} alltests[] = {
    { test_change_event },
    { test_backend_registry },
    { test_redis_parse },
    { test_redis_equivalence },
    { NULL },
};

int main(int argc, const char *const *argv)
{
    int i;
    abts_suite *suite = NULL;

    ogs_core_initialize();
    atexit(ogs_core_terminate);

    ogs_log_install_domain(&__ogs_dbi_domain, "dbi", OGS_LOG_ERROR);

    for (i = 0; alltests[i].func; i++)
        suite = alltests[i].func(suite);

    return abts_report(suite);
}
