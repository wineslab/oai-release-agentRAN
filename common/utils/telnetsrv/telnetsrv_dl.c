/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TELNETSERVERCODE
#include "telnetsrv.h"

#include "openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.h"
#include "openair2/LAYER2/NR_MAC_gNB/mac_proto.h"

#define ERROR_MSG_RET(mSG, aRGS...) do { prnt(mSG, ##aRGS); return 1; } while (0)

static int dl_load_scheduler_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: dl load_scheduler <script_path>\n");

    while (*buff == ' ' || *buff == '\t') buff++;
    char path[512];
    if (sscanf(buff, "%511s", path) != 1)
        ERROR_MSG_RET("Error: expected <script_path>\n");

    FILE *f = fopen(path, "r");
    if (!f)
        ERROR_MSG_RET("Error: cannot open file '%s'\n", path);
    fclose(f);

    reload_lua_dl_scheduler(path);
    prnt("Lua DL scheduler reloaded from '%s'\n", path);
    return 0;
}

static int dl_scheduler_config_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: dl scheduler_config <fwa_max_throughput> <mtc_max_throughput>\n"
                      "  values in Mbps (0 = no limit)\n");

    int fwa_max, mtc_max;
    if (sscanf(buff, "%d %d", &fwa_max, &mtc_max) != 2)
        ERROR_MSG_RET("Error: expected <fwa_max_throughput> <mtc_max_throughput>\n");

    if (fwa_max < 0 || mtc_max < 0)
        ERROR_MSG_RET("Error: throughput limits must be >= 0 (0 = no limit)\n");

    set_lua_dl_scheduler_config(fwa_max, mtc_max);
    prnt("DL scheduler config updated: fwa_max=%d Mbps, mtc_max=%d Mbps\n", fwa_max, mtc_max);
    return 0;
}

telnetshell_cmddef_t dl_cmdarray[] = {
    {"load_scheduler",   "<script_path>",                            dl_load_scheduler_cmd,   {NULL}, 0, NULL},
    {"scheduler_config", "<fwa_max_throughput> <mtc_max_throughput>", dl_scheduler_config_cmd, {NULL}, 0, NULL},
    {"", "", NULL, {NULL}, 0, NULL},
};

telnetshell_vardef_t dl_vardef[] = {
    {"", 0, 0, NULL}
};

void add_dl_cmds(void)
{
    add_telnetcmd("dl", dl_vardef, dl_cmdarray);
}
