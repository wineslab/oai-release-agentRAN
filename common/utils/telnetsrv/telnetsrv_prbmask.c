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
#include "common/utils/nr/nr_common.h"

#define ERROR_MSG_RET(mSG, aRGS...) do { prnt(mSG, ##aRGS); return 1; } while (0)

static int prbmask_set_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask apply <mask_string>  ('.'=allowed, 'X'=blocked, max %d)\n", MAX_BWP_SIZE);

    while (*buff == ' ' || *buff == '\t') buff++;
    int mask_len = strlen(buff);
    if (mask_len > MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: mask length (%d) exceeds max BWP size (%d)\n", mask_len, MAX_BWP_SIZE);

    for (int i = 0; i < mask_len; i++)
        if (buff[i] != '.' && buff[i] != 'X')
            ERROR_MSG_RET("Error: invalid character '%c' at position %d (only '.' and 'X' allowed)\n", buff[i], i);

    set_blocked_prb_mask(buff);
    int blocked = 0;
    for (int i = 0; i < mask_len; i++)
        if (buff[i] == 'X') blocked++;
    prnt("PRB mask set: %d/%d PRBs blocked\n", blocked, mask_len);
    return 0;
}

static int prbmask_get_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    int display_len = (int)strlen(g_blocked_prb_mask);
    if (display_len == 0) {
        prnt("PRB mask not initialized\n");
        return 0;
    }
    int blocked = 0;
    for (int i = 0; i < display_len; i++)
        if (g_blocked_prb_mask[i] == 'X') blocked++;
    prnt("Blocked PRBs: %d/%d\n", blocked, display_len);
    prnt("Mask: %.*s\n", display_len, g_blocked_prb_mask);
    return 0;
}

static int prbmask_clear_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    clear_blocked_prb_mask();
    prnt("PRB mask cleared (all PRBs allowed)\n");
    return 0;
}

static int prbmask_block_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask blockrb <start_prb> <num_prbs>\n");
    int start_prb, num_prbs;
    if (sscanf(buff, "%d %d", &start_prb, &num_prbs) != 2)
        ERROR_MSG_RET("Error: expected two integers: <start_prb> <num_prbs>\n");
    if (start_prb < 0 || start_prb >= MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: start_prb (%d) out of range [0, %d)\n", start_prb, MAX_BWP_SIZE);
    if (num_prbs <= 0)
        ERROR_MSG_RET("Error: num_prbs must be positive\n");
    if (start_prb + num_prbs > MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: range [%d, %d) exceeds max BWP size %d\n", start_prb, start_prb + num_prbs, MAX_BWP_SIZE);

    block_prb_range(start_prb, num_prbs);
    prnt("Blocked PRBs [%d, %d) (%d PRBs)\n", start_prb, start_prb + num_prbs, num_prbs);
    return 0;
}

static int prbmask_unblock_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask unblockrb <start_prb> <num_prbs>\n");
    int start_prb, num_prbs;
    if (sscanf(buff, "%d %d", &start_prb, &num_prbs) != 2)
        ERROR_MSG_RET("Error: expected two integers: <start_prb> <num_prbs>\n");
    if (start_prb < 0 || start_prb >= MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: start_prb (%d) out of range [0, %d)\n", start_prb, MAX_BWP_SIZE);
    if (num_prbs <= 0)
        ERROR_MSG_RET("Error: num_prbs must be positive\n");
    if (start_prb + num_prbs > MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: range [%d, %d) exceeds max BWP size %d\n", start_prb, start_prb + num_prbs, MAX_BWP_SIZE);

    unblock_prb_range(start_prb, num_prbs);
    prnt("Unblocked PRBs [%d, %d) (%d PRBs)\n", start_prb, start_prb + num_prbs, num_prbs);
    return 0;
}

/* --- Per-UE PRB blocking commands (by uid) --- */

static int prbmask_ueblock_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask ueblock <uid> <start_prb> <num_prbs>\n");
    unsigned int uid;
    int start_prb, num_prbs;
    if (sscanf(buff, "%u %d %d", &uid, &start_prb, &num_prbs) != 3)
        ERROR_MSG_RET("Error: expected <uid> <start_prb> <num_prbs>\n");

    gNB_MAC_INST *mac = RC.nrmac[0];
    if (!mac)
        ERROR_MSG_RET("Error: MAC instance not found\n");
    NR_UE_info_t *UE = find_nr_UE_by_uid(&mac->UE_info, uid);
    if (!UE)
        ERROR_MSG_RET("Error: UE with uid %u not found\n", uid);
    if (start_prb < 0 || start_prb >= MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: start_prb (%d) out of range [0, %d)\n", start_prb, MAX_BWP_SIZE);
    if (num_prbs <= 0)
        ERROR_MSG_RET("Error: num_prbs must be positive\n");
    if (start_prb + num_prbs > MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: range [%d, %d) exceeds max BWP size %d\n", start_prb, start_prb + num_prbs, MAX_BWP_SIZE);

    block_ue_prb_range(UE, start_prb, num_prbs);
    prnt("UE uid %u (RNTI 0x%04x): blocked PRBs [%d, %d) (%d PRBs)\n", uid, UE->rnti, start_prb, start_prb + num_prbs, num_prbs);
    return 0;
}

static int prbmask_ueunblock_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask ueunblock <uid> <start_prb> <num_prbs>\n");
    unsigned int uid;
    int start_prb, num_prbs;
    if (sscanf(buff, "%u %d %d", &uid, &start_prb, &num_prbs) != 3)
        ERROR_MSG_RET("Error: expected <uid> <start_prb> <num_prbs>\n");

    gNB_MAC_INST *mac = RC.nrmac[0];
    if (!mac)
        ERROR_MSG_RET("Error: MAC instance not found\n");
    NR_UE_info_t *UE = find_nr_UE_by_uid(&mac->UE_info, uid);
    if (!UE)
        ERROR_MSG_RET("Error: UE with uid %u not found\n", uid);
    if (start_prb < 0 || start_prb >= MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: start_prb (%d) out of range [0, %d)\n", start_prb, MAX_BWP_SIZE);
    if (num_prbs <= 0)
        ERROR_MSG_RET("Error: num_prbs must be positive\n");
    if (start_prb + num_prbs > MAX_BWP_SIZE)
        ERROR_MSG_RET("Error: range [%d, %d) exceeds max BWP size %d\n", start_prb, start_prb + num_prbs, MAX_BWP_SIZE);

    unblock_ue_prb_range(UE, start_prb, num_prbs);
    prnt("UE uid %u (RNTI 0x%04x): unblocked PRBs [%d, %d) (%d PRBs)\n", uid, UE->rnti, start_prb, start_prb + num_prbs, num_prbs);
    return 0;
}

static int prbmask_ueapply_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask ueapply <uid> <mask_string>\n");
    unsigned int uid;
    char mask[MAX_BWP_SIZE + 1];
    if (sscanf(buff, "%u %275s", &uid, mask) != 2)
        ERROR_MSG_RET("Error: expected <uid> <mask_string>\n");

    gNB_MAC_INST *mac = RC.nrmac[0];
    if (!mac)
        ERROR_MSG_RET("Error: MAC instance not found\n");
    NR_UE_info_t *UE = find_nr_UE_by_uid(&mac->UE_info, uid);
    if (!UE)
        ERROR_MSG_RET("Error: UE with uid %u not found\n", uid);

    int mask_len = strlen(mask);
    for (int i = 0; i < mask_len; i++)
        if (mask[i] != '.' && mask[i] != 'X')
            ERROR_MSG_RET("Error: invalid character '%c' at position %d\n", mask[i], i);

    set_ue_blocked_prb_mask(UE, mask);
    int blocked = 0;
    for (int i = 0; i < mask_len; i++)
        if (mask[i] == 'X') blocked++;
    prnt("UE uid %u (RNTI 0x%04x): PRB mask set, %d/%d PRBs blocked\n", uid, UE->rnti, blocked, mask_len);
    return 0;
}

static int prbmask_ueshow_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    gNB_MAC_INST *mac = RC.nrmac[0];
    if (!mac)
        ERROR_MSG_RET("Error: MAC instance not found\n");

    if (!buff || strlen(buff) == 0) {
        prnt("=== Per-UE PRB Masks ===\n");
        int count = 0;
        UE_iterator(mac->UE_info.connected_ue_list, UE) {
            int display_len = (int)strlen(UE->ul_blocked_prb_mask);
            int blocked = 0;
            for (int i = 0; i < display_len; i++)
                if (UE->ul_blocked_prb_mask[i] == 'X') blocked++;
            prnt("UE uid %u (RNTI 0x%04x): %d/%d PRBs blocked\n", UE->uid, UE->rnti, blocked, display_len);
            if (blocked > 0)
                prnt("  Mask: %.*s\n", display_len, UE->ul_blocked_prb_mask);
            count++;
        }
        if (count == 0)
            prnt("No connected UEs\n");
    } else {
        while (*buff == ' ' || *buff == '\t') buff++;
        unsigned int uid = (unsigned int)strtoul(buff, NULL, 10);
        NR_UE_info_t *UE = find_nr_UE_by_uid(&mac->UE_info, uid);
        if (!UE)
            ERROR_MSG_RET("Error: UE with uid %u not found\n", uid);
        int display_len = (int)strlen(UE->ul_blocked_prb_mask);
        int blocked = 0;
        for (int i = 0; i < display_len; i++)
            if (UE->ul_blocked_prb_mask[i] == 'X') blocked++;
        prnt("UE uid %u (RNTI 0x%04x): %d/%d PRBs blocked\n", uid, UE->rnti, blocked, display_len);
        prnt("Mask: %.*s\n", display_len, UE->ul_blocked_prb_mask);
    }
    return 0;
}

static int prbmask_ueclear_cmd(char *buff, int debug, telnet_printfunc_t prnt)
{
    if (!buff || strlen(buff) == 0)
        ERROR_MSG_RET("Usage: prbmask ueclear <uid>\n");
    while (*buff == ' ' || *buff == '\t') buff++;
    unsigned int uid = (unsigned int)strtoul(buff, NULL, 10);

    gNB_MAC_INST *mac = RC.nrmac[0];
    if (!mac)
        ERROR_MSG_RET("Error: MAC instance not found\n");
    NR_UE_info_t *UE = find_nr_UE_by_uid(&mac->UE_info, uid);
    if (!UE)
        ERROR_MSG_RET("Error: UE with uid %u not found\n", uid);

    clear_ue_blocked_prb_mask(UE);
    prnt("UE uid %u (RNTI 0x%04x): PRB mask cleared (all PRBs allowed)\n", uid, UE->rnti);
    return 0;
}

telnetshell_cmddef_t prbmask_cmdarray[] = {
    {"apply",     "<mask_string>",          prbmask_set_cmd,       {NULL}, 0, NULL},
    {"show",      "",                       prbmask_get_cmd,       {NULL}, 0, NULL},
    {"clear",     "",                       prbmask_clear_cmd,     {NULL}, 0, NULL},
    {"blockrb",   "<start_prb> <num_prbs>", prbmask_block_cmd,     {NULL}, 0, NULL},
    {"unblockrb", "<start_prb> <num_prbs>", prbmask_unblock_cmd,   {NULL}, 0, NULL},
    {"ueblock",   "<uid> <start> <num>",    prbmask_ueblock_cmd,   {NULL}, 0, NULL},
    {"ueunblock", "<uid> <start> <num>",    prbmask_ueunblock_cmd, {NULL}, 0, NULL},
    {"ueapply",   "<uid> <mask_string>",    prbmask_ueapply_cmd,   {NULL}, 0, NULL},
    {"ueshow",    "[uid]",                  prbmask_ueshow_cmd,    {NULL}, 0, NULL},
    {"ueclear",   "<uid>",                  prbmask_ueclear_cmd,   {NULL}, 0, NULL},
    {"", "", NULL, {NULL}, 0, NULL},
};

telnetshell_vardef_t prbmask_vardef[] = {
    {"", 0, 0, NULL}
};

void add_prbmask_cmds(void)
{
    add_telnetcmd("prbmask", prbmask_vardef, prbmask_cmdarray);
}
