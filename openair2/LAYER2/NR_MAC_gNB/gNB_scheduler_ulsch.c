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

/*! \file gNB_scheduler_ulsch.c
 * \brief gNB procedures for the ULSCH transport channel
 * \author Navid Nikaein and Raymond Knopp, Guido Casati
 * \date 2019
 * \email: guido.casati@iis.fraunhofer.de
 * \version 1.0
 * @ingroup _mac
 */


#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "executables/softmodem-common.h"
#include "common/utils/nr/nr_common.h"
#include "utils.h"
#include <openair2/UTIL/OPT/opt.h>
#include "LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include <math.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

//#define SRS_IND_DEBUG

static int tpc_to_db(int tpc_command) {
    switch (tpc_command) {
        case 0: return -1;
        case 1: return 0;
        case 2: return 1;
        case 3: return 3;
        default:
            LOG_W(NR_MAC, "Invalid TPC command %d, using 0 dB\n", tpc_command);
            return 0;
    }
}

/* \brief Get the number of UL TDAs that could be used in slot, reachable
 * via specific k2. The output parameter first_idx is a pointer to the first
 * suitable TDA, and the function returns the number of suitable TDAs, or 0. */
int get_num_ul_tda(gNB_MAC_INST *nrmac, int slot, int k2, const NR_tda_info_t **first_idx)
{
  /* we assume that this function is mutex-protected from outside */
  NR_SCHED_ENSURE_LOCKED(&nrmac->sched_lock);

  const uint16_t ul_bitmap = get_ul_bitmap(&nrmac->frame_structure, slot);
  *first_idx = NULL;
  FOR_EACH_SEQ_ARR(NR_tda_info_t *, tda, &nrmac->ul_tda) {
    DevAssert(tda->valid_tda);
    // nr_rrc_config_ul_tda() orders by k2, so skip smaller and return for
    // bigger ones
    if (tda->k2 < k2)
      continue;
    if (tda->k2 > k2)
      break; // there won't be a suitable k2 anymore

    uint16_t tda_bitmap = SL_to_bitmap(tda->startSymbolIndex, tda->nrOfSymbols);
    // nr_rrc_config_ul_tda() assures TDAs are from largest to smallest symbol number.
    // check that this TDA fits entirely into this slot's mask (slot might be
    // smaller in mixed slots)., A mixed slot TDA would be checked last, so a
    // full slot TDA will be used for a full UL slot.
    if ((tda_bitmap & ul_bitmap) == tda_bitmap) { // if TDA fits entiry
      *first_idx = tda;
      break;
    }
  }
  if (*first_idx == NULL) /* nothing fit */
    return 0;

  NR_tda_info_t *end_it = seq_arr_next(&nrmac->ul_tda, *first_idx);
  while (end_it != seq_arr_end(&nrmac->ul_tda) && end_it->k2 == k2) {
    /* the following TDAs should all fit as long as the k2 is the same */
    uint16_t tda_bitmap = SL_to_bitmap(end_it->startSymbolIndex, end_it->nrOfSymbols);
    AssertFatal((tda_bitmap & ul_bitmap) == tda_bitmap,
                "TDA should fit inside slot, but is not the case for k2 %ld bitmap 0x%04x\n",
                end_it->k2,
                tda_bitmap);
    end_it = seq_arr_next(&nrmac->ul_tda, end_it);
  }

  ptrdiff_t diff = seq_arr_dist(&nrmac->ul_tda, *first_idx, end_it);
  AssertFatal(diff > 0 && diff <= seq_arr_size(&nrmac->ul_tda), "dist %ld\n", diff);
  return diff;
}

static void get_max_rb_range(const uint16_t *vrb_map_ul, const uint16_t *ulprbbl, uint16_t mask, int *rb_start, int *rb_len)
{
  int best_start = *rb_start;
  int best_len = 0;
  int current_start = *rb_start;
  int current_len = 0;
  for (int rb = *rb_start; rb < *rb_len; ++rb) {
    if (ulprbbl[rb] == 0 && (mask & vrb_map_ul[rb]) == 0) {
      current_len++;
      continue;
    }

    /* this RB is blocked for UL, or already in use */
    if (current_len > best_len) {
      best_start = current_start;
      best_len = current_len;
    }
    current_start = rb + 1; /* will start in next RB, or updated later */
    current_len = 0;
  }
  if (current_len > best_len) {
    best_start = current_start;
    best_len = current_len;
  }
  *rb_start = best_start;
  *rb_len = best_len;
}

const NR_tda_info_t *get_best_ul_tda(const gNB_MAC_INST *nrmac, int beam, const NR_tda_info_t *tdas, int n_tda, int frame, int slot, int *rb_start, int *rb_len)
{
  /* there is a mixed slot only when in TDD */
  const frame_structure_t *fs = &nrmac->frame_structure;
  const int index = ul_buffer_index(frame, slot, fs->numb_slots_frame, nrmac->vrb_map_UL_size);
  uint16_t *vrb_map_UL = &nrmac->common_channels[0].vrb_map_UL[beam][index * MAX_BWP_SIZE];

  DevAssert(n_tda <= 16);
  const NR_tda_info_t *best_tda = tdas;
  uint64_t score = 0;
  int check_rb_start = *rb_start;
  int check_rb_len = *rb_len;

  for (int i = 0; i < n_tda; ++i, tdas++) {
    int start = check_rb_start;
    int len = check_rb_len;
    uint16_t tda_mask = SL_to_bitmap(tdas->startSymbolIndex, tdas->nrOfSymbols);
    get_max_rb_range(vrb_map_UL, nrmac->ulprbbl, tda_mask, &start, &len);
    uint64_t s = (uint64_t)tdas->nrOfSymbols * len;
    if (s > score) {
      best_tda = tdas;
      score = s;
      *rb_start = start;
      *rb_len = len;
    }
    LOG_D(NR_MAC, "%4d.%2d tda k2 %ld mask 0x%04x has PRB start/len %d/%d score %ld\n", frame, slot, tdas->k2, tda_mask, start, len, s);
  }

  return best_tda;
}

bwp_info_t get_pusch_bwp_start_size(NR_UE_info_t *UE)
{
  NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  bwp_info_t bwp_info;
  bwp_info.bwpStart = ul_bwp->BWPStart;

  // 3GPP TS 38.214 Section 6.1.2.2.2 Uplink resource allocation type 1
  // In uplink resource allocation of type 1, the resource block assignment information indicates to a scheduled UE a set of
  // contiguously allocated non-interleaved virtual resource blocks within the active bandwidth part of size   PRBs except for the
  // case when DCI format 0_0 is decoded in any common search space in which case the size of the initial UL bandwidth part shall
  // be used.
  if (ul_bwp->dci_format == NR_UL_DCI_FORMAT_0_0
      && sched_ctrl->search_space->searchSpaceType
      && sched_ctrl->search_space->searchSpaceType->present == NR_SearchSpace__searchSpaceType_PR_common) {
    bwp_info.bwpSize = min(ul_bwp->BWPSize, UE->sc_info.initial_ul_BWPSize);
  } else {
    bwp_info.bwpSize = ul_bwp->BWPSize;
  }
  return bwp_info;
}

static int compute_ph_factor(int mu, int tbs_bits, int rb, int n_layers, int n_symbols, int n_dmrs, long *deltaMCS, bool include_bw)
{
  // 38.213 7.1.1
  // if the PUSCH transmission is over more than one layer delta_tf = 0
  float delta_tf = 0;
  if(deltaMCS != NULL && n_layers == 1) {
    const int n_re = (NR_NB_SC_PER_RB * n_symbols - n_dmrs) * rb;
    const float BPRE = (float) tbs_bits/n_re;  //TODO change for PUSCH with CSI
    const float f = pow(2, BPRE * 1.25);
    const float beta = 1.0f; //TODO change for PUSCH with CSI
    delta_tf = (10 * log10((f - 1) * beta));
    LOG_D(NR_MAC,
          "PH factor delta_tf %f (n_re %d, n_rb %d, n_dmrs %d, n_symbols %d, tbs %d BPRE %f f %f)\n",
          delta_tf,
          n_re,
          rb,
          n_dmrs,
          n_symbols,
          tbs_bits,
          BPRE,
          f);
  }
  const float bw_factor = (include_bw) ? 10 * log10(rb << mu) : 0;
  return ((int)roundf(delta_tf + bw_factor));
}

/* \brief over-estimate the BSR index, given real_index.
 *
 * BSR does not account for headers, so we need to estimate. See 38.321
 * 6.1.3.1: "The size of the RLC headers and MAC subheaders are not considered
 * in the buffer size computation." */
static int overestim_bsr_index(int real_index)
{
  /* if UE reports BSR 0, it means "no data"; otherwise, overestimate to
   * account for headers */
  const int add_overestim = 1;
  return real_index > 0 ? real_index + add_overestim : real_index;
}

static int estimate_ul_buffer_short_bsr(const NR_BSR_SHORT *bsr)
{
  /* NOTE: the short BSR might be for different LCGID than 0, but we do not
   * differentiate them */
  int rep_idx = bsr->Buffer_size;
  int estim_idx = overestim_bsr_index(rep_idx);
  int max = NR_SHORT_BSR_TABLE_SIZE - 1;
  int idx = min(estim_idx, max);
  int estim_size = get_short_bsr_value(idx);
  LOG_D(NR_MAC, "short BSR LCGID %d index %d estim index %d size %d\n", bsr->LcgID, rep_idx, estim_idx, estim_size);
  return estim_size;
}

static int estimate_ul_buffer_long_bsr(const NR_BSR_LONG *bsr)
{
  LOG_D(NR_MAC,
        "LONG BSR, LCG ID(7-0) %d/%d/%d/%d/%d/%d/%d/%d\n",
        bsr->LcgID7,
        bsr->LcgID6,
        bsr->LcgID5,
        bsr->LcgID4,
        bsr->LcgID3,
        bsr->LcgID2,
        bsr->LcgID1,
        bsr->LcgID0);
  bool bsr_active[8] = {bsr->LcgID0 != 0, bsr->LcgID1 != 0, bsr->LcgID2 != 0, bsr->LcgID3 != 0, bsr->LcgID4 != 0, bsr->LcgID5 != 0, bsr->LcgID6 != 0, bsr->LcgID7 != 0};

  int estim_size = 0;
  int max = NR_LONG_BSR_TABLE_SIZE - 1;
  uint8_t *payload = ((uint8_t*) bsr) + 1;
  int m = 0;
  const int total_lcgids = 8; /* see 38.321 6.1.3.1 */
  for (int n = 0; n < total_lcgids; n++) {
    if (!bsr_active[n])
      continue;
    int rep_idx = payload[m];
    int estim_idx = overestim_bsr_index(rep_idx);
    int idx = min(estim_idx, max);
    estim_size += get_long_bsr_value(idx);

    LOG_D(NR_MAC, "LONG BSR LCGID/m %d/%d Index %d estim index %d size %d", n, m, rep_idx, estim_idx, estim_size);
    m++;
  }
  return estim_size;
}

//  For both UL-SCH except:
//   - UL-SCH: fixed-size MAC CE(known by LCID)
//   - UL-SCH: padding
//   - UL-SCH: MSG3 48-bits
//  |0|1|2|3|4|5|6|7|  bit-wise
//  |R|F|   LCID    |
//  |       L       |
//  |0|1|2|3|4|5|6|7|  bit-wise
//  |R|F|   LCID    |
//  |       L       |
//  |       L       |
//
//  For:
//   - UL-SCH: fixed-size MAC CE(known by LCID)
//   - UL-SCH: padding, for single/multiple 1-oct padding CE(s)
//   - UL-SCH: MSG3 48-bits
//  |0|1|2|3|4|5|6|7|  bit-wise
//  |R|R|   LCID    |
//
//  LCID: The Logical Channel ID field identifies the logical channel instance of the corresponding MAC SDU or the type of the corresponding MAC CE or padding as described in Tables 6.2.1-1 and 6.2.1-2 for the DL-SCH and UL-SCH respectively. There is one LCID field per MAC subheader. The LCID field size is 6 bits;
//  L: The Length field indicates the length of the corresponding MAC SDU or variable-sized MAC CE in bytes. There is one L field per MAC subheader except for subheaders corresponding to fixed-sized MAC CEs and padding. The size of the L field is indicated by the F field;
//  F: length of L is 0:8 or 1:16 bits wide
//  R: Reserved bit, set to zero.

// return: length of subPdu header
// 3GPP TS 38.321 Section 6
uint8_t decode_ul_mac_sub_pdu_header(uint8_t *pduP, uint8_t *lcid, uint16_t *length)
{
  uint16_t mac_subheader_len = 1;
  *lcid = pduP[0] & 0x3F;

  switch (*lcid) {
    case UL_SCH_LCID_CCCH_64_BITS:
      *length = 8;
      break;
    case UL_SCH_LCID_SRB1:
    case UL_SCH_LCID_SRB2:
    case UL_SCH_LCID_DTCH ...(UL_SCH_LCID_DTCH + 28):
    case UL_SCH_LCID_L_TRUNCATED_BSR:
    case UL_SCH_LCID_L_BSR:
      if (pduP[0] & 0x40) { // F = 1
        mac_subheader_len = 3;
        *length = (pduP[1] << 8) + pduP[2];
      } else { // F = 0
        mac_subheader_len = 2;
        *length = pduP[1];
      }
      break;
    case UL_SCH_LCID_CCCH_48_BITS_REDCAP:
    case UL_SCH_LCID_CCCH_48_BITS:
      *length = 6;
      break;
    case UL_SCH_LCID_SINGLE_ENTRY_PHR:
    case UL_SCH_LCID_C_RNTI:
      *length = 2;
      break;
    case UL_SCH_LCID_S_TRUNCATED_BSR:
    case UL_SCH_LCID_S_BSR:
      *length = 1;
      break;
    case UL_SCH_LCID_PADDING:
      // Nothing to do
      break;
    default:
      LOG_E(NR_MAC, "LCID %0x not handled yet!\n", *lcid);
      break;
  }

  LOG_D(NR_MAC, "Decoded LCID 0x%X, header bytes: %d, payload bytes %d\n", *lcid, mac_subheader_len, *length);

  return mac_subheader_len;
}

static rnti_t lcid_crnti_lookahead(uint8_t *pdu, uint32_t pdu_len)
{
  while (pdu_len > 0) {
    uint16_t mac_len = 0;
    uint8_t lcid = 0;
    uint16_t mac_subheader_len = decode_ul_mac_sub_pdu_header(pdu, &lcid, &mac_len);
    // Check for valid PDU
    if (mac_subheader_len + mac_len > pdu_len) {
      LOG_E(NR_MAC,
            "Invalid PDU! mac_subheader_len: %d, mac_len: %d, remaining pdu_len: %d\n",
            mac_subheader_len,
            mac_len,
            pdu_len);

      LOG_E(NR_MAC, "Residual UL MAC PDU: ");
      uint32_t print_len = pdu_len > 30 ? 30 : pdu_len; // Only printf 1st - 30nd bytes
      for (int i = 0; i < print_len; i++)
        printf("%02x ", pdu[i]);
      printf("\n");
      return 0;
    }

    if (lcid == UL_SCH_LCID_C_RNTI) {
      // Extract C-RNTI value
      rnti_t crnti = ((pdu[1] & 0xFF) << 8) | (pdu[2] & 0xFF);
      LOG_A(NR_MAC, "Received a MAC CE for C-RNTI with %04x\n", crnti);
      return crnti;
    } else if (lcid == UL_SCH_LCID_PADDING) {
      // End of MAC PDU, can ignore the remaining bytes
      return 0;
    }

    pdu += mac_len + mac_subheader_len;
    pdu_len -= mac_len + mac_subheader_len;
  }
  return 0;
}

static int nr_process_mac_pdu(instance_t module_idP,
                              NR_UE_info_t *UE,
                              uint8_t CC_id,
                              frame_t frameP,
                              slot_t slot,
                              uint8_t *pduP,
                              uint32_t pdu_len,
                              const int8_t harq_pid)
{
  int sdus = 0;
  NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

  if (pduP[0] != UL_SCH_LCID_PADDING) {
    ws_trace_t tmp = {.nr = true,
                      .direction = DIRECTION_UPLINK,
                      .pdu_buffer = pduP,
                      .pdu_buffer_size = pdu_len,
                      .ueid = 0,
                      .rntiType = WS_C_RNTI,
                      .rnti = UE->rnti,
                      .sysFrame = frameP,
                      .subframe = slot,
                      .harq_pid = harq_pid};
    trace_pdu(&tmp);
  }

#ifdef ENABLE_MAC_PAYLOAD_DEBUG
  LOG_I(NR_MAC, "In %s: dumping MAC PDU in %d.%d:\n", __func__, frameP, slot);
  log_dump(NR_MAC, pduP, pdu_len, LOG_DUMP_CHAR, "\n");
#endif

  while (pdu_len > 0) {
    uint16_t mac_len = 0;
    uint8_t lcid = 0;

    uint16_t mac_subheader_len = decode_ul_mac_sub_pdu_header(pduP, &lcid, &mac_len);
    // Check for valid PDU
    if (mac_subheader_len + mac_len > pdu_len) {
      LOG_E(NR_MAC,
            "Invalid PDU in %d.%d for RNTI %04X! mac_subheader_len: %d, mac_len: %d, remaining pdu_len: %d\n",
            frameP,
            slot,
            UE->rnti,
            mac_subheader_len,
            mac_len,
            pdu_len);

      LOG_E(NR_MAC, "Residual UL MAC PDU: ");
      int print_len = pdu_len > 30 ? 30 : pdu_len; // Only printf 1st - 30nd bytes
      for (int i = 0; i < print_len; i++)
        printf("%02x ", pduP[i]);
      printf("\n");
      return 0;
    }

    LOG_D(NR_MAC,
          "Received UL-SCH sub-PDU with LCID 0x%X in %d.%d for RNTI %04X (remaining PDU length %d)\n",
          lcid,
          frameP,
          slot,
          UE->rnti,
          pdu_len);

    unsigned char *ce_ptr;

    switch (lcid) {
      case UL_SCH_LCID_CCCH_64_BITS:
      case UL_SCH_LCID_CCCH_48_BITS_REDCAP:
      case UL_SCH_LCID_CCCH_48_BITS:
        if (lcid == UL_SCH_LCID_CCCH_64_BITS) {
          // Check if it is a valid CCCH1 message, we get all 00's messages very often
          bool valid_pdu = false;
          for (int i = 0; i < (mac_subheader_len + mac_len); i++) {
            if (pduP[i] != 0) {
              valid_pdu = true;
              break;
            }
          }
          if (!valid_pdu) {
            LOG_D(NR_MAC, "%s() Invalid CCCH1 message!, pdu_len: %d\n", __func__, pdu_len);
            return 0;
          }
        }

        LOG_I(MAC, "[RAPROC] Received SDU for CCCH length %d for UE %04x\n", mac_len, UE->rnti);

        if (lcid == UL_SCH_LCID_CCCH_48_BITS_REDCAP) {
          LOG_I(MAC, "UE with RNTI %04x is RedCap\n", UE->rnti);
          UE->is_redcap = true;
        }

        if (prepare_initial_ul_rrc_message(RC.nrmac[module_idP], UE)) {
          nr_mac_rlc_data_ind(module_idP, UE->rnti, true, 0, (char *)(pduP + mac_subheader_len), mac_len);
        } else {
          LOG_E(NR_MAC, "prepare_initial_ul_rrc_message() returned false, cannot forward CCCH message\n");
        }
        break;

      case UL_SCH_LCID_SRB1:
      case UL_SCH_LCID_SRB2:
        AssertFatal(UE->CellGroup,
                    "UE %04x %d.%d: Received LCID %d which is not configured (UE has no CellGroup)\n",
                    UE->rnti,
                    frameP,
                    slot,
                    lcid);

        const nr_lc_config_t *srbc = nr_mac_get_lc_config(sched_ctrl, lcid);
        if (!srbc || srbc->suspended) {
          LOG_I(NR_MAC, "RNTI %04x LCID %d: ignoring %d bytes\n", UE->rnti, lcid, mac_len);
        } else {
          nr_mac_rlc_data_ind(module_idP, UE->rnti, true, lcid, (char *)(pduP + mac_subheader_len), mac_len);

          UE->mac_stats.ul.total_sdu_bytes += mac_len;
          UE->mac_stats.ul.lc_bytes[lcid] += mac_len;
        }
        T(T_GNB_MAC_LCID_UL, T_INT(UE->rnti), T_INT(frameP), T_INT(slot), T_INT(lcid), T_INT(mac_len * 8));
        break;

      case UL_SCH_LCID_DTCH ...(UL_SCH_LCID_DTCH + 28):
        LOG_D(NR_MAC,
              "[UE %04x] %d.%d : ULSCH -> UL-%s %d (gNB %ld, %d bytes)\n",
              UE->rnti,
              frameP,
              slot,
              lcid < 4 ? "DCCH" : "DTCH",
              lcid,
              module_idP,
              mac_len);
        const nr_lc_config_t *c = nr_mac_get_lc_config(sched_ctrl, lcid);
        if (!c || c->suspended) {
          LOG_I(NR_MAC, "RNTI %04x LCID %d: ignoring %d bytes\n", UE->rnti, lcid, mac_len);
        } else {
          UE->mac_stats.ul.lc_bytes[lcid] += mac_len;
          UE->mac_stats.ul.total_sdu_bytes += mac_len;

          nr_mac_rlc_data_ind(module_idP, UE->rnti, true, lcid, (char *)(pduP + mac_subheader_len), mac_len);

          sdus += 1;
          /* Updated estimated buffer when receiving data */
          if (sched_ctrl->estimated_ul_buffer >= mac_len)
            sched_ctrl->estimated_ul_buffer -= mac_len;
          else
            sched_ctrl->estimated_ul_buffer = 0;
        }
        T(T_GNB_MAC_LCID_UL, T_INT(UE->rnti), T_INT(frameP), T_INT(slot), T_INT(lcid), T_INT(mac_len * 8));
        break;

      case UL_SCH_LCID_RECOMMENDED_BITRATE_QUERY:
        // 38.321 Ch6.1.3.20
        break;

      case UL_SCH_LCID_MULTI_ENTRY_PHR_4_OCT:
        LOG_E(NR_MAC, "Multi entry PHR not supported\n");
        break;

      case UL_SCH_LCID_CONFIGURED_GRANT_CONFIRMATION:
        // 38.321 Ch6.1.3.7
        break;

      case UL_SCH_LCID_MULTI_ENTRY_PHR_1_OCT:
        LOG_E(NR_MAC, "Multi entry PHR not supported\n");
        break;

      case UL_SCH_LCID_SINGLE_ENTRY_PHR:
        if (harq_pid < 0) {
          LOG_E(NR_MAC, "Invalid HARQ PID %d\n", harq_pid);
          return 0;
        }
        NR_sched_pusch_t *sched_pusch = &sched_ctrl->ul_harq_processes[harq_pid].sched_pusch;

        /* Extract SINGLE ENTRY PHR elements for PHR calculation */
        ce_ptr = &pduP[mac_subheader_len];
        NR_SINGLE_ENTRY_PHR_MAC_CE *phr = (NR_SINGLE_ENTRY_PHR_MAC_CE *)ce_ptr;
        /* Save the phr info */
        int PH;
        const int PCMAX = phr->PCMAX;
        /* 38.133 Table10.1.17.1-1 */
        if (phr->PH < 55) {
          PH = phr->PH - 32;
        } else if (phr->PH < 63) {
          PH = 24 + (phr->PH - 55) * 2;
        } else {
          PH = 38;
        }
        // in sched_ctrl we set normalized PH wrt MCS and PRBs
        long *deltaMCS = ul_bwp->pusch_Config ? ul_bwp->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
        sched_ctrl->ph = PH
                         + compute_ph_factor(ul_bwp->scs,
                                             sched_pusch->tb_size << 3,
                                             sched_pusch->rbSize,
                                             sched_pusch->nrOfLayers,
                                             sched_pusch->tda_info.nrOfSymbols, // n_symbols
                                             sched_pusch->dmrs_info.num_dmrs_symb * sched_pusch->dmrs_info.N_PRB_DMRS, // n_dmrs
                                             deltaMCS,
                                             true);
        sched_ctrl->ph0 = PH;
        /* 38.133 Table10.1.18.1-1 */
        sched_ctrl->pcmax = PCMAX - 29;
        LOG_D(NR_MAC,
              "SINGLE ENTRY PHR %d.%d R1 %d PH %d (%d dB) R2 %d PCMAX %d (%d dBm)\n",
              frameP,
              slot,
              phr->R1,
              PH,
              sched_ctrl->ph,
              phr->R2,
              PCMAX,
              sched_ctrl->pcmax);
        break;

      case UL_SCH_LCID_C_RNTI:
        if (UE->ra && UE->ra->ra_state == nrRA_gNB_IDLE) {
          // Extract C-RNTI value
          rnti_t crnti = ((pduP[1] & 0xFF) << 8) | (pduP[2] & 0xFF);
          AssertFatal(false,
                      "Received MAC CE for C-RNTI %04x without RA running, procedure exists? Or is it a bug while decoding the "
                      "MAC PDU?\n",
                      crnti);
        }
        break;

      case UL_SCH_LCID_S_TRUNCATED_BSR:
      case UL_SCH_LCID_S_BSR:
        /* Extract short BSR value */
        ce_ptr = &pduP[mac_subheader_len];
        sched_ctrl->estimated_ul_buffer = estimate_ul_buffer_short_bsr((NR_BSR_SHORT *)ce_ptr);
        sched_ctrl->sched_ul_bytes = 0;
        LOG_D(NR_MAC, "SHORT BSR at %4d.%2d, est buf %d\n", frameP, slot, sched_ctrl->estimated_ul_buffer);
        break;

      case UL_SCH_LCID_L_TRUNCATED_BSR:
      case UL_SCH_LCID_L_BSR:
        /* Extract long BSR value */
        ce_ptr = &pduP[mac_subheader_len];
        sched_ctrl->estimated_ul_buffer = estimate_ul_buffer_long_bsr((NR_BSR_LONG *)ce_ptr);
        sched_ctrl->sched_ul_bytes = 0;
        LOG_D(NR_MAC, "LONG BSR at %4d.%2d, estim buf %d\n", frameP, slot, sched_ctrl->estimated_ul_buffer);
        break;

      case UL_SCH_LCID_PADDING:
        // End of MAC PDU, can ignore the rest.
        return 0;

      default:
        LOG_E(NR_MAC, "RNTI %0x [%d.%d], received unknown MAC header (LCID = 0x%02x)\n", UE->rnti, frameP, slot, lcid);
        return -1;
        break;
    }

#ifdef ENABLE_MAC_PAYLOAD_DEBUG
    if (lcid < 45 || lcid == 52 || lcid == 63) {
      LOG_I(NR_MAC, "In %s: dumping UL MAC SDU sub-header with length %d (LCID = 0x%02x):\n", __func__, mac_subheader_len, lcid);
      log_dump(NR_MAC, pduP, mac_subheader_len, LOG_DUMP_CHAR, "\n");
      LOG_I(NR_MAC, "In %s: dumping UL MAC SDU with length %d (LCID = 0x%02x):\n", __func__, mac_len, lcid);
      log_dump(NR_MAC, pduP + mac_subheader_len, mac_len, LOG_DUMP_CHAR, "\n");
    } else {
      LOG_I(NR_MAC, "In %s: dumping UL MAC CE with length %d (LCID = 0x%02x):\n", __func__, mac_len, lcid);
      log_dump(NR_MAC, pduP + mac_subheader_len + mac_len, mac_len, LOG_DUMP_CHAR, "\n");
    }
#endif

    pduP += (mac_subheader_len + mac_len);
    pdu_len -= (mac_subheader_len + mac_len);
  }

  UE->mac_stats.ul.num_mac_sdu += sdus;

  return 0;
}

static void finish_nr_ul_harq(NR_UE_sched_ctrl_t *sched_ctrl, int harq_pid)
{
  NR_UE_ul_harq_t *harq = &sched_ctrl->ul_harq_processes[harq_pid];

  harq->ndi ^= 1;
  harq->round = 0;

  add_tail_nr_list(&sched_ctrl->available_ul_harq, harq_pid);
}

static void abort_nr_ul_harq(NR_UE_info_t *UE, int8_t harq_pid)
{
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_ul_harq_t *harq = &sched_ctrl->ul_harq_processes[harq_pid];

  finish_nr_ul_harq(sched_ctrl, harq_pid);
  UE->mac_stats.ul.errors++;

  /* the transmission failed: the UE won't send the data we expected initially,
   * so retrieve to correctly schedule after next BSR */
  sched_ctrl->sched_ul_bytes -= harq->sched_pusch.tb_size;
  if (sched_ctrl->sched_ul_bytes < 0)
    sched_ctrl->sched_ul_bytes = 0;
}

static void handle_nr_ul_harq(gNB_MAC_INST *nrmac,
                              NR_UE_info_t *UE,
                              frame_t frame,
                              slot_t slot,
                              rnti_t rnti,
                              int crc_harq_id,
                              bool crc_status)
{
  if (nrmac->radio_config.disable_harq) {
    LOG_D(NR_MAC, "skipping UL feedback handling as HARQ is disabled\n");
    return;
  }

  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  int8_t harq_pid = sched_ctrl->feedback_ul_harq.head;
  LOG_D(NR_MAC, "Comparing crc harq_id vs feedback harq_pid = %d %d\n", crc_harq_id, harq_pid);
  while (crc_harq_id != harq_pid || harq_pid < 0) {
    LOG_W(NR_MAC, "Unexpected ULSCH HARQ PID %d (have %d) for RNTI 0x%04x\n", crc_harq_id, harq_pid, rnti);
    if (harq_pid < 0)
      return;

    remove_front_nr_list(&sched_ctrl->feedback_ul_harq);
    sched_ctrl->ul_harq_processes[harq_pid].is_waiting = false;

    if(sched_ctrl->ul_harq_processes[harq_pid].round >= nrmac->ul_bler.harq_round_max - 1) {
      abort_nr_ul_harq(UE, harq_pid);
    } else {
      sched_ctrl->ul_harq_processes[harq_pid].round++;
      add_tail_nr_list(&sched_ctrl->retrans_ul_harq, harq_pid);
    }
    harq_pid = sched_ctrl->feedback_ul_harq.head;
  }
  remove_front_nr_list(&sched_ctrl->feedback_ul_harq);
  NR_UE_ul_harq_t *harq = &sched_ctrl->ul_harq_processes[harq_pid];
  DevAssert(harq->is_waiting);
  harq->feedback_slot = -1;
  harq->is_waiting = false;
  if (!crc_status) {
    finish_nr_ul_harq(sched_ctrl, harq_pid);
    LOG_D(NR_MAC,
          "Ulharq id %d crc passed for RNTI %04x\n",
          harq_pid,
          rnti);
  } else if (harq->round >= nrmac->ul_bler.harq_round_max  - 1) {
    abort_nr_ul_harq(UE, harq_pid);
    LOG_D(NR_MAC,
          "RNTI %04x: Ulharq id %d crc failed in all rounds\n",
          rnti,
          harq_pid);
  } else {
    harq->round++;
    LOG_D(NR_MAC,
          "Ulharq id %d crc failed for RNTI %04x\n",
          harq_pid,
          rnti);
    add_tail_nr_list(&sched_ctrl->retrans_ul_harq, harq_pid);
  }
}

static void handle_msg3_failed_rx(gNB_MAC_INST *mac, NR_RA_t *ra, rnti_t rnti, int harq_round_max)
{
  if (ra->msg3_round >= harq_round_max - 1) {
    LOG_W(NR_MAC, "UE %04x RA failed at state %s (Reached msg3 max harq rounds)\n", rnti, nrra_text[ra->ra_state]);
    nr_release_ra_UE(mac, rnti);
    return;
  }

  LOG_D(NR_MAC, "UE %04x Msg3 CRC did not pass\n", rnti);
  ra->msg3_round++;
  ra->ra_state = nrRA_Msg3_retransmission;
}

static void nr_rx_ra_sdu(const module_id_t mod_id,
                         const int CC_id,
                         const frame_t frame,
                         const sub_frame_t slot,
                         rnti_t rnti,
                         uint8_t *sdu,
                         const uint32_t sdu_len,
                         uint8_t harq_pid,
                         const uint16_t timing_advance,
                         const uint8_t ul_cqi,
                         const uint16_t rssi)
{
  gNB_MAC_INST *mac = RC.nrmac[mod_id];
  NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  NR_UE_info_t *UE = find_ra_UE(&mac->UE_info, rnti);
  if (!UE) {
    LOG_E(NR_MAC, "UL SDU discarded. Couldn't finde UE with RNTI %04x \n", rnti);
    return;
  }

  NR_RA_t *ra = UE->ra;
  if (ra->ra_type == RA_4_STEP && ra->ra_state != nrRA_WAIT_Msg3) {
    LOG_W(NR_MAC, "UL SDU discarded for RNTI %04x RA state not waiting for PUSCH\n", UE->rnti);
    return;
  }

  // CFRA: we scheduled Msg3 (which does not exist in CFRA, see also
  // nr_generate_Msg2()). We did not mark RA as complete right away, as the
  // DLSCH scheduler might schedule in the same slot as Msg2 if RLC has data
  // (which can only happen in do-ra), so we mark it as complete now.
  bool cfra = ra->cfra;
  if (ra->cfra) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    nr_mac_reset_link_failure(sched_ctrl);
    reset_dl_harq_list(sched_ctrl);
    reset_ul_harq_list(sched_ctrl);
    // we configure the UE using dedicated search space: In SA (CFRA used for
    // handover) and NSA (or do-ra), the UE has the full config already.
    int ss_type = NR_SearchSpace__searchSpaceType_PR_ue_Specific;
    configure_UE_BWP(mac, scc, UE, false, ss_type, -1, -1);
    // initialize ta_frame in case there is no Msg3 received
    UE->UE_sched_ctrl.ta_frame = (frame + 100) % MAX_FRAME_NUMBER;
    if (!transition_ra_connected_nr_ue(mac, UE)) {
      LOG_E(NR_MAC, "cannot add UE %04x: list is full\n", UE->rnti);
      delete_nr_ue_data(UE, NULL, &mac->UE_info.uid_allocator);
    } else {
      LOG_A(NR_MAC, "(rnti 0x%04x) CFRA procedure succeeded!\n", UE->rnti);
      UE->UE_sched_ctrl.SR = true;
    }
  }

  const int target_snrx10 = mac->pusch_target_snrx10;
  UE->UE_sched_ctrl.pusch_target_snrx10 = mac->pusch_target_snrx10;

  if (!sdu) { // NACK
    if (cfra)  // no Msg3 on CFRA, no problem
      return;

    if (ra->ra_state != nrRA_WAIT_Msg3)
      return;

    if((frame!=ra->Msg3_frame) || (slot!=ra->Msg3_slot))
      return;

    if (ul_cqi != 0xff)
      ra->msg3_TPC = nr_get_tpc(target_snrx10, ul_cqi, 30, 0);

    handle_msg3_failed_rx(mac, ra, rnti, mac->ul_bler.harq_round_max);
    return;
  }


  bool no_sig = true;
  for (uint32_t k = 0; k < sdu_len; k++) {
    if (sdu[k] != 0) {
      no_sig = false;
      break;
    }
  }

  T(T_GNB_MAC_UL_PDU_WITH_DATA, T_INT(mod_id), T_INT(CC_id),
    T_INT(rnti), T_INT(frame), T_INT(slot), T_INT(-1) /* harq_pid */,
    T_BUFFER(sdu, sdu_len));


  if (no_sig) {
    LOG_W(NR_MAC, "MSG3 ULSCH with no signal\n");
    if (!cfra)
      handle_msg3_failed_rx(mac, ra, rnti, mac->ul_bler.harq_round_max);
    return;
  }
  if (!cfra && ra->ra_type == RA_2_STEP) {
    // random access pusch with RA-RNTI
    if (ra->RA_rnti != rnti) {
      LOG_E(NR_MAC, "expected TC_RNTI %04x to match current RNTI %04x\n", ra->RA_rnti, rnti);
      return;
    }
  }

  // re-initialize ta update variables after RA procedure completion
  UE->UE_sched_ctrl.ta_frame = (frame + 100) % MAX_FRAME_NUMBER;

  LOG_A(NR_MAC, "%4d.%2d PUSCH with TC_RNTI 0x%04x received correctly\n", frame, slot, rnti);

  NR_UE_sched_ctrl_t *UE_scheduling_control = &UE->UE_sched_ctrl;
  DevAssert(harq_pid >= 0 && harq_pid < 8);
  if (ul_cqi != 0xff) {
    NR_UE_ul_harq_t *harq = &UE_scheduling_control->ul_harq_processes[harq_pid];
    UE_scheduling_control->tpc0 = nr_get_tpc(target_snrx10, ul_cqi, 30, harq->sched_pusch.phr_txpower_calc);
    UE_scheduling_control->pusch_snrx10 = ul_cqi * 5 - 640 - harq->sched_pusch.phr_txpower_calc * 10;
  }
  if (timing_advance != 0xffff)
    UE_scheduling_control->ta_update = timing_advance;
  UE_scheduling_control->raw_rssi = rssi;
  LOG_D(NR_MAC, "[UE %04x] PUSCH TPC %d and TA %d\n", UE->rnti, UE_scheduling_control->tpc0, UE_scheduling_control->ta_update);

  LOG_D(NR_MAC, "[RAPROC] Received %s:\n", ra->ra_type == RA_2_STEP ? "MsgA-PUSCH" : "Msg3");
  for (uint32_t k = 0; k < sdu_len; k++) {
    LOG_D(NR_MAC, "(%i): 0x%x\n", k, sdu[k]);
  }

  // 3GPP TS 38.321 Section 5.4.3 Multiplexing and assembly
  // Logical channels shall be prioritised in accordance with the following order (highest priority listed first):
  // - MAC CE for C-RNTI, or data from UL-CCCH;
  // This way, we need to process MAC CE for C-RNTI if RA is active and it is present in the MAC PDU
  // Search for MAC CE for C-RNTI
  rnti_t crnti = lcid_crnti_lookahead(sdu, sdu_len);
  if (crnti != 0) { // 3GPP TS 38.321 Table 7.1-1: RNTI values, RNTI 0x0000: N/A
    // Replace the current UE by the UE identified by C-RNTI
    NR_UE_info_t *old_UE = find_nr_UE(&mac->UE_info, crnti);
    if (!old_UE) {
      // The UE identified by C-RNTI no longer exists at the gNB
      // Let's abort the current RA, so the UE will trigger a new RA later but using RRCSetupRequest instead. A better
      // solution may be implemented
      LOG_W(NR_MAC, "No UE found with C-RNTI %04x, ignoring Msg3 to have UE come back with new RA attempt\n", UE->rnti);
      nr_release_ra_UE(mac, rnti);
      return;
    }
    // in case UE beam has changed
    old_UE->UE_beam_index = UE->UE_beam_index;
    // Reset UL failure for old UE
    nr_mac_reset_link_failure(&old_UE->UE_sched_ctrl);
    // Reset HARQ processes
    reset_dl_harq_list(&old_UE->UE_sched_ctrl);
    reset_ul_harq_list(&old_UE->UE_sched_ctrl);

    // Only trigger RRCReconfiguration if UE is not performing RRCReestablishment
    // The RRCReconfiguration will be triggered by the RRCReestablishmentComplete
    if (!old_UE->reconfigCellGroup) {
      LOG_I(NR_MAC, "Received UL_SCH_LCID_C_RNTI with C-RNTI 0x%04x, triggering RRC Reconfiguration\n", crnti);
      // Trigger RRCReconfiguration
      nr_mac_trigger_reconfiguration(mac, old_UE, -1);
      // we configure the UE using common search space with DCIX0 while waiting for a reconfiguration
      configure_UE_BWP(mac, scc, old_UE, false, NR_SearchSpace__searchSpaceType_PR_common, -1, -1);
    }
    nr_release_ra_UE(mac, rnti);
    LOG_A(NR_MAC, "%4d.%2d RA with C-RNTI %04x complete\n", frame, slot, crnti);

    // Decode the entire MAC PDU
    // It may have multiple MAC subPDUs, for example, a MAC subPDU with LCID 1 caring a RRCReestablishmentComplete
    nr_process_mac_pdu(mod_id, old_UE, CC_id, frame, slot, sdu, sdu_len, -1);
    return;
  }

  if (cfra)
    return; // rest not relevant for CFRA

  // UE Contention Resolution Identity
  // Store the first 48 bits belonging to the uplink CCCH SDU within Msg3 to fill in Msg4
  // First byte corresponds to R/LCID MAC sub-header
  memcpy(ra->cont_res_id, &sdu[1], sizeof(uint8_t) * 6);

  // Decode MAC PDU
  // the function is only called to decode the contention resolution sub-header
  // harq_pid set a non-valid value because it is not used in this call
  nr_process_mac_pdu(mod_id, UE, CC_id, frame, slot, sdu, sdu_len, -1);

  LOG_I(NR_MAC,
        "Activating scheduling %s for TC_RNTI 0x%04x (state %s)\n",
        ra->ra_type == RA_2_STEP ? "MsgB" : "Msg4",
        UE->rnti,
        nrra_text[ra->ra_state]);
  ra->ra_state = ra->ra_type == RA_2_STEP ? nrRA_MsgB : nrRA_Msg4;
  LOG_D(NR_MAC, "TC_RNTI 0x%04x next RA state %s\n", UE->rnti, nrra_text[ra->ra_state]);
  return;
}

static void _nr_rx_sdu(const module_id_t gnb_mod_idP,
                       const int CC_idP,
                       const frame_t frameP,
                       const slot_t slotP,
                       const rnti_t rntiP,
                       uint8_t *sduP,
                       const uint32_t sdu_lenP,
                       const int8_t harq_pid,
                       const uint16_t timing_advance,
                       const uint8_t ul_cqi,
                       const uint16_t rssi)
{
  gNB_MAC_INST *gNB_mac = RC.nrmac[gnb_mod_idP];
  const int current_rnti = rntiP;
  LOG_D(NR_MAC, "rx_sdu for rnti %04x\n", current_rnti);
  const int rssi_threshold = gNB_mac->pusch_rssi_threshold;
  const int pusch_failure_thres = gNB_mac->pusch_failure_thres;
  NR_UE_info_t *UE = find_nr_UE(&gNB_mac->UE_info, current_rnti);
  if (UE) {
    const int target_snrx10 = (UE->ul_snr_target_override >= 0)
        ? UE->ul_snr_target_override
        : gNB_mac->pusch_target_snrx10;
    NR_UE_sched_ctrl_t *UE_scheduling_control = &UE->UE_sched_ctrl;
    UE_scheduling_control->pusch_target_snrx10 = target_snrx10;
    if (sduP)
      T(T_GNB_MAC_UL_PDU_WITH_DATA, T_INT(gnb_mod_idP), T_INT(CC_idP),
        T_INT(rntiP), T_INT(frameP), T_INT(slotP), T_INT(harq_pid),
        T_BUFFER(sduP, sdu_lenP));

    UE->mac_stats.ul.total_bytes += sdu_lenP;
    LOG_D(NR_MAC, "[gNB %d][PUSCH %d] CC_id %d %d.%d Received ULSCH sdu from PHY (rnti %04x) ul_cqi %d TA %d sduP %p, rssi %d\n",
          gnb_mod_idP,
          harq_pid,
          CC_idP,
          frameP,
          slotP,
          current_rnti,
          ul_cqi,
          timing_advance,
          sduP,
          rssi);

    // EWMA filtering of channel-only CQI (remove TPC effect before filtering)
    if (ul_cqi != 0xff) {
      double channel_only_cqi = (double)ul_cqi - (UE_scheduling_control->accumulated_tpc_db * 2);
      if (!UE_scheduling_control->ul_cqi_initialized) {
        UE_scheduling_control->filtered_ul_cqi = channel_only_cqi;
        UE_scheduling_control->ul_cqi_initialized = true;
      } else {
        UE_scheduling_control->filtered_ul_cqi = 0.95 * UE_scheduling_control->filtered_ul_cqi
                                                + 0.05 * channel_only_cqi;
      }
    }

    // Compute filtered CQI: add TPC back to get actual SNR estimate
    uint8_t filtered_cqi = ul_cqi;
    if (UE_scheduling_control->ul_cqi_initialized) {
      filtered_cqi = (uint8_t)round(UE_scheduling_control->filtered_ul_cqi
                                    + (UE_scheduling_control->accumulated_tpc_db * 2));
    }

    if (harq_pid < 0) {
      LOG_E(NR_MAC, "UE %04x received ULSCH when feedback UL HARQ %d (unexpected ULSCH transmission)\n", rntiP, harq_pid);
      return;
    }

    // if not missed detection (10dB threshold for now)
    if (rssi > 0) {
      int txpower_calc = UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.phr_txpower_calc;
      UE->mac_stats.deltaMCS = txpower_calc;
      UE->mac_stats.NPRB = UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.rbSize;
      if (ul_cqi != 0xff)
        UE_scheduling_control->tpc0 = nr_get_tpc(target_snrx10, filtered_cqi, 30, txpower_calc);
      if (UE_scheduling_control->ph < 0 && UE_scheduling_control->tpc0 > 1)
        UE_scheduling_control->tpc0 = 1;

      UE_scheduling_control->tpc0 = nr_limit_tpc(UE_scheduling_control->tpc0, rssi, rssi_threshold);

      if (timing_advance != 0xffff)
        UE_scheduling_control->ta_update = timing_advance;
      UE_scheduling_control->raw_rssi = rssi;
      UE_scheduling_control->pusch_snrx10 = filtered_cqi * 5 - 640 - (txpower_calc * 10);
      if (UE_scheduling_control->tpc0 > 1)
        LOG_D(NR_MAC,
              "[UE %04x] %d.%d. PUSCH TPC %d and TA %d pusch_snrx10 %d rssi %d phrx_tx_power %d PHR (1PRB) %d mcs %d, nb_rb %d\n",
              UE->rnti,
              frameP,
              slotP,
              UE_scheduling_control->tpc0,
              UE_scheduling_control->ta_update,
              UE_scheduling_control->pusch_snrx10,
              UE_scheduling_control->raw_rssi,
              txpower_calc,
              UE_scheduling_control->ph,
              UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.mcs,
              UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.rbSize);

      NR_UE_ul_harq_t *cur_harq = &UE_scheduling_control->ul_harq_processes[harq_pid];
      if (cur_harq->round == 0)
        UE->mac_stats.pusch_snrx10 = UE_scheduling_control->pusch_snrx10;
      LOG_D(NR_MAC, "[UE %04x] PUSCH TPC %d and TA %d\n",UE->rnti,UE_scheduling_control->tpc0,UE_scheduling_control->ta_update);
    }
    else{
      LOG_D(NR_MAC,"[UE %04x] Detected DTX : increasing UE TX power\n",UE->rnti);
      UE_scheduling_control->tpc0 = 1;
    }

#if defined(ENABLE_MAC_PAYLOAD_DEBUG)

    LOG_I(NR_MAC, "Printing received UL MAC payload at gNB side: %d \n");
    for (uint32_t i = 0; i < sdu_lenP; i++) {
      // harq_process_ul_ue->a[i] = (unsigned char) rand();
      // printf("a[%d]=0x%02x\n",i,harq_process_ul_ue->a[i]);
      printf("%02x ", (unsigned char)sduP[i]);
    }
    printf("\n");

#endif

    if (sduP != NULL) {
      LOG_D(NR_MAC, "Received PDU at MAC gNB \n");
      UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt = 0;
      UE_scheduling_control->sched_ul_bytes -= sdu_lenP;
      if (UE_scheduling_control->sched_ul_bytes < 0)
        UE_scheduling_control->sched_ul_bytes = 0;

      nr_process_mac_pdu(gnb_mod_idP, UE, CC_idP, frameP, slotP, sduP, sdu_lenP, harq_pid);
    } else {
      if (ul_cqi == 0xff || ul_cqi <= 128) {
        UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt++;
        UE->mac_stats.ulsch_DTX++;
      }

      if (!get_softmodem_params()->phy_test && UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt >= pusch_failure_thres) {
        LOG_W(NR_MAC,
              "%4d.%2d UE %04x: Detected UL Failure on PUSCH after %d PUSCH DTX, stopping scheduling\n",
              frameP,
              slotP,
              UE->rnti,
              UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt);
        nr_mac_trigger_link_failure(&UE->UE_sched_ctrl, UE->current_UL_BWP.scs);
      }
    }
    handle_nr_ul_harq(gNB_mac, UE, frameP, slotP, current_rnti, harq_pid, sduP == NULL);
  } else { 
    nr_rx_ra_sdu(gnb_mod_idP, CC_idP, frameP, slotP, current_rnti, sduP, sdu_lenP, harq_pid, timing_advance, ul_cqi, rssi);
  }
}

void nr_rx_sdu(const module_id_t gnb_mod_idP,
               const int CC_idP,
               const frame_t frameP,
               const slot_t slotP,
               const rnti_t rntiP,
               uint8_t *sduP,
               const uint32_t sdu_lenP,
               const int8_t harq_pid,
               const uint16_t timing_advance,
               const uint8_t ul_cqi,
               const uint16_t rssi)
{
  gNB_MAC_INST *gNB_mac = RC.nrmac[gnb_mod_idP];
  NR_SCHED_LOCK(&gNB_mac->sched_lock);
  start_meas(&gNB_mac->rx_ulsch_sdu);
  _nr_rx_sdu(gnb_mod_idP, CC_idP, frameP, slotP, rntiP, sduP, sdu_lenP, harq_pid, timing_advance, ul_cqi, rssi);
  stop_meas(&gNB_mac->rx_ulsch_sdu);
  NR_SCHED_UNLOCK(&gNB_mac->sched_lock);
}

static uint32_t calc_power_complex(const int16_t *x, const int16_t *y, const uint32_t size)
{
  // Real part value
  int64_t sum_x = 0;
  int64_t sum_x2 = 0;
  for(int k = 0; k<size; k++) {
    sum_x = sum_x + x[k];
    sum_x2 = sum_x2 + x[k]*x[k];
  }
  uint32_t power_re = sum_x2/size - (sum_x/size)*(sum_x/size);

  // Imaginary part power
  int64_t sum_y = 0;
  int64_t sum_y2 = 0;
  for(int k = 0; k<size; k++) {
    sum_y = sum_y + y[k];
    sum_y2 = sum_y2 + y[k]*y[k];
  }
  uint32_t power_im = sum_y2/size - (sum_y/size)*(sum_y/size);

  return power_re+power_im;
}

static c16_t nr_h_times_w(c16_t h, char w)
{
  c16_t output;
    switch (w) {
      case '0': // 0
        output.r = 0;
        output.i = 0;
        break;
      case '1': // 1
        output.r = h.r;
        output.i = h.i;
        break;
      case 'n': // -1
        output.r = -h.r;
        output.i = -h.i;
        break;
      case 'j': // j
        output.r = -h.i;
        output.i = h.r;
        break;
      case 'o': // -j
        output.r = h.i;
        output.i = -h.r;
        break;
      default:
        AssertFatal(1==0,"Invalid precoder value %c\n", w);
    }
  return output;
}

static uint8_t get_max_tpmi(const NR_PUSCH_Config_t *pusch_Config,
                            const uint16_t num_ue_srs_ports,
                            const uint8_t *nrOfLayers,
                            int *additional_max_tpmi)
{
  uint8_t max_tpmi = 0;

  if (!pusch_Config
      || (pusch_Config->txConfig != NULL && *pusch_Config->txConfig == NR_PUSCH_Config__txConfig_nonCodebook)
      || num_ue_srs_ports == 1)
    return max_tpmi;

  long max_rank = *pusch_Config->maxRank;
  long *ul_FullPowerTransmission = pusch_Config->ext1 ? pusch_Config->ext1->ul_FullPowerTransmission_r16 : NULL;
  long *codebookSubset = pusch_Config->codebookSubset;

  if (num_ue_srs_ports == 2) {

    if (max_rank == 1) {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        max_tpmi = 2;
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = 1;
        } else {
          max_tpmi = 5;
        }
      }
    } else {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        max_tpmi = *nrOfLayers == 1 ? 2 : 0;
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = *nrOfLayers == 1 ? 1 : 0;
        } else {
          max_tpmi = *nrOfLayers == 1 ? 5 : 2;
        }
      }
    }

  } else if (num_ue_srs_ports == 4) {

    if (max_rank == 1) {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = 3;
          *additional_max_tpmi = 13;
        } else {
          max_tpmi = 15;
        }
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = 3;
        } else if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_partialAndNonCoherent) {
          max_tpmi = 11;
        } else {
          max_tpmi = 27;
        }
      }
    } else {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        if (max_rank == 2) {
          if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
            max_tpmi = *nrOfLayers == 1 ? 3 : 6;
            if (*nrOfLayers == 1) {
              *additional_max_tpmi = 13;
            }
          } else {
            max_tpmi = *nrOfLayers == 1 ? 15 : 13;
          }
        } else {
          if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
            switch (*nrOfLayers) {
              case 1:
                max_tpmi = 3;
                *additional_max_tpmi = 13;
                break;
              case 2:
                max_tpmi = 6;
                break;
              case 3:
                max_tpmi = 1;
                break;
              case 4:
                max_tpmi = 0;
                break;
              default:
                LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
            }
          } else {
            switch (*nrOfLayers) {
              case 1:
                max_tpmi = 15;
                break;
              case 2:
                max_tpmi = 13;
                break;
              case 3:
              case 4:
                max_tpmi = 2;
                break;
              default:
                LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
            }
          }
        }
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          switch (*nrOfLayers) {
            case 1:
              max_tpmi = 3;
              break;
            case 2:
              max_tpmi = 5;
              break;
            case 3:
            case 4:
              max_tpmi = 0;
              break;
            default:
              LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
          }
        } else if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_partialAndNonCoherent) {
          switch (*nrOfLayers) {
            case 1:
              max_tpmi = 11;
              break;
            case 2:
              max_tpmi = 13;
              break;
            case 3:
            case 4:
              max_tpmi = 2;
              break;
            default:
              LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
          }
        } else {
          switch (*nrOfLayers) {
            case 1:
              max_tpmi = 28;
              break;
            case 2:
              max_tpmi = 22;
              break;
            case 3:
              max_tpmi = 7;
              break;
            case 4:
              max_tpmi = 5;
              break;
            default:
              LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
          }
        }
      }
    }

  }

  return max_tpmi;
}

// TS 38.211 - Table 6.3.1.5-1: Precoding matrix W for single-layer transmission using two antenna ports, 'n' = -1 and 'o' = -j
const char table_38211_6_3_1_5_1[6][2][1] = {
    {{'1'}, {'0'}}, // tpmi 0
    {{'0'}, {'1'}}, // tpmi 1
    {{'1'}, {'1'}}, // tpmi 2
    {{'1'}, {'n'}}, // tpmi 3
    {{'1'}, {'j'}}, // tpmi 4
    {{'1'}, {'o'}}  // tpmi 5
};

// TS 38.211 - Table 6.3.1.5-2: Precoding matrix W for single-layer transmission using four antenna ports with transform precoding enabled, 'n' = -1 and 'o' = -j
const char table_38211_6_3_1_5_2[28][4][1] = {
    {{'1'}, {'0'}, {'0'}, {'0'}}, // tpmi 0
    {{'0'}, {'1'}, {'0'}, {'0'}}, // tpmi 1
    {{'0'}, {'0'}, {'1'}, {'0'}}, // tpmi 2
    {{'0'}, {'0'}, {'0'}, {'1'}}, // tpmi 3
    {{'1'}, {'0'}, {'1'}, {'0'}}, // tpmi 4
    {{'1'}, {'0'}, {'n'}, {'0'}}, // tpmi 5
    {{'1'}, {'0'}, {'j'}, {'0'}}, // tpmi 6
    {{'1'}, {'0'}, {'o'}, {'0'}}, // tpmi 7
    {{'0'}, {'1'}, {'0'}, {'1'}}, // tpmi 8
    {{'0'}, {'1'}, {'0'}, {'n'}}, // tpmi 9
    {{'0'}, {'1'}, {'0'}, {'j'}}, // tpmi 10
    {{'0'}, {'1'}, {'0'}, {'o'}}, // tpmi 11
    {{'1'}, {'1'}, {'1'}, {'n'}}, // tpmi 12
    {{'1'}, {'1'}, {'j'}, {'j'}}, // tpmi 13
    {{'1'}, {'1'}, {'n'}, {'1'}}, // tpmi 14
    {{'1'}, {'1'}, {'o'}, {'o'}}, // tpmi 15
    {{'1'}, {'j'}, {'1'}, {'j'}}, // tpmi 16
    {{'1'}, {'j'}, {'j'}, {'1'}}, // tpmi 17
    {{'1'}, {'j'}, {'n'}, {'o'}}, // tpmi 18
    {{'1'}, {'j'}, {'o'}, {'n'}}, // tpmi 19
    {{'1'}, {'n'}, {'1'}, {'1'}}, // tpmi 20
    {{'1'}, {'n'}, {'j'}, {'o'}}, // tpmi 21
    {{'1'}, {'n'}, {'n'}, {'n'}}, // tpmi 22
    {{'1'}, {'n'}, {'o'}, {'j'}}, // tpmi 23
    {{'1'}, {'o'}, {'1'}, {'o'}}, // tpmi 24
    {{'1'}, {'o'}, {'j'}, {'n'}}, // tpmi 25
    {{'1'}, {'o'}, {'n'}, {'j'}}, // tpmi 26
    {{'1'}, {'o'}, {'o'}, {'1'}}  // tpmi 27
};

// TS 38.211 - Table 6.3.1.5-3: Precoding matrix W for single-layer transmission using four antenna ports with transform precoding disabled, 'n' = -1 and 'o' = -j
const char table_38211_6_3_1_5_3[28][4][1] = {
    {{'1'}, {'0'}, {'0'}, {'0'}}, // tpmi 0
    {{'0'}, {'1'}, {'0'}, {'0'}}, // tpmi 1
    {{'0'}, {'0'}, {'1'}, {'0'}}, // tpmi 2
    {{'0'}, {'0'}, {'0'}, {'1'}}, // tpmi 3
    {{'1'}, {'0'}, {'1'}, {'0'}}, // tpmi 4
    {{'1'}, {'0'}, {'n'}, {'0'}}, // tpmi 5
    {{'1'}, {'0'}, {'j'}, {'0'}}, // tpmi 6
    {{'1'}, {'0'}, {'o'}, {'0'}}, // tpmi 7
    {{'0'}, {'1'}, {'0'}, {'1'}}, // tpmi 8
    {{'0'}, {'1'}, {'0'}, {'n'}}, // tpmi 9
    {{'0'}, {'1'}, {'0'}, {'j'}}, // tpmi 10
    {{'0'}, {'1'}, {'0'}, {'o'}}, // tpmi 11
    {{'1'}, {'1'}, {'1'}, {'1'}}, // tpmi 12
    {{'1'}, {'1'}, {'j'}, {'j'}}, // tpmi 13
    {{'1'}, {'1'}, {'n'}, {'n'}}, // tpmi 14
    {{'1'}, {'1'}, {'o'}, {'o'}}, // tpmi 15
    {{'1'}, {'j'}, {'1'}, {'j'}}, // tpmi 16
    {{'1'}, {'j'}, {'j'}, {'n'}}, // tpmi 17
    {{'1'}, {'j'}, {'n'}, {'o'}}, // tpmi 18
    {{'1'}, {'j'}, {'o'}, {'1'}}, // tpmi 19
    {{'1'}, {'n'}, {'1'}, {'n'}}, // tpmi 20
    {{'1'}, {'n'}, {'j'}, {'o'}}, // tpmi 21
    {{'1'}, {'n'}, {'n'}, {'1'}}, // tpmi 22
    {{'1'}, {'n'}, {'o'}, {'j'}}, // tpmi 23
    {{'1'}, {'o'}, {'1'}, {'o'}}, // tpmi 24
    {{'1'}, {'o'}, {'j'}, {'1'}}, // tpmi 25
    {{'1'}, {'o'}, {'n'}, {'j'}}, // tpmi 26
    {{'1'}, {'o'}, {'o'}, {'n'}}  // tpmi 27
};

// TS 38.211 - Table 6.3.1.5-4: Precoding matrix W for two-layer transmission using two antenna ports, 'n' = -1 and 'o' = -j
const char table_38211_6_3_1_5_4[3][2][2] = {
    {{'1', '0'}, {'0', '1'}}, // tpmi 0
    {{'1', '1'}, {'1', 'n'}}, // tpmi 1
    {{'1', '1'}, {'j', 'o'}}  // tpmi 2
};

// TS 38.211 - Table 6.3.1.5-5: Precoding matrix W for two-layer transmission using four antenna ports, 'n' = -1 and 'o' = -j
const char table_38211_6_3_1_5_5[22][4][2] = {
    {{'1', '0'}, {'0', '1'}, {'0', '0'}, {'0', '0'}}, // tpmi 0
    {{'1', '0'}, {'0', '0'}, {'0', '1'}, {'0', '0'}}, // tpmi 1
    {{'1', '0'}, {'0', '0'}, {'0', '0'}, {'0', '1'}}, // tpmi 2
    {{'0', '0'}, {'1', '0'}, {'0', '1'}, {'0', '0'}}, // tpmi 3
    {{'0', '0'}, {'1', '0'}, {'0', '0'}, {'0', '1'}}, // tpmi 4
    {{'0', '0'}, {'0', '0'}, {'1', '0'}, {'0', '1'}}, // tpmi 5
    {{'1', '0'}, {'0', '1'}, {'1', '0'}, {'0', 'o'}}, // tpmi 6
    {{'1', '0'}, {'0', '1'}, {'1', '0'}, {'0', 'j'}}, // tpmi 7
    {{'1', '0'}, {'0', '1'}, {'o', '0'}, {'0', '1'}}, // tpmi 8
    {{'1', '0'}, {'0', '1'}, {'o', '0'}, {'0', 'n'}}, // tpmi 9
    {{'1', '0'}, {'0', '1'}, {'n', '0'}, {'0', 'o'}}, // tpmi 10
    {{'1', '0'}, {'0', '1'}, {'n', '0'}, {'0', 'j'}}, // tpmi 11
    {{'1', '0'}, {'0', '1'}, {'j', '0'}, {'0', '1'}}, // tpmi 12
    {{'1', '0'}, {'0', '1'}, {'j', '0'}, {'0', 'n'}}, // tpmi 13
    {{'1', '1'}, {'1', '1'}, {'1', 'n'}, {'1', 'n'}}, // tpmi 14
    {{'1', '1'}, {'1', '1'}, {'j', 'o'}, {'j', 'o'}}, // tpmi 15
    {{'1', '1'}, {'j', 'j'}, {'1', 'n'}, {'j', 'o'}}, // tpmi 16
    {{'1', '1'}, {'j', 'j'}, {'j', 'o'}, {'n', '1'}}, // tpmi 17
    {{'1', '1'}, {'n', 'n'}, {'1', 'n'}, {'n', '1'}}, // tpmi 18
    {{'1', '1'}, {'n', 'n'}, {'j', 'o'}, {'o', 'j'}}, // tpmi 19
    {{'1', '1'}, {'o', 'o'}, {'1', 'n'}, {'o', 'j'}}, // tpmi 20
    {{'1', '1'}, {'o', 'o'}, {'j', 'o'}, {'1', 'n'}}  // tpmi 21
};

static void get_precoder_matrix_coef(char *w,
                                     const uint8_t ul_ri,
                                     const uint16_t num_ue_srs_ports,
                                     const long transform_precoding,
                                     const uint8_t tpmi,
                                     const uint8_t uI,
                                     int layer_idx)
{
  if (ul_ri == 0) {
    if (num_ue_srs_ports == 2) {
      *w = table_38211_6_3_1_5_1[tpmi][uI][layer_idx];
    } else {
      if (transform_precoding == NR_PUSCH_Config__transformPrecoder_enabled) {
        *w = table_38211_6_3_1_5_2[tpmi][uI][layer_idx];
      } else {
        *w = table_38211_6_3_1_5_3[tpmi][uI][layer_idx];
      }
    }
  } else if (ul_ri == 1) {
    if (num_ue_srs_ports == 2) {
      *w = table_38211_6_3_1_5_4[tpmi][uI][layer_idx];
    } else {
      *w = table_38211_6_3_1_5_5[tpmi][uI][layer_idx];
    }
  } else {
    AssertFatal(1 == 0, "Function get_precoder_matrix_coef() does not support %i layers yet!\n", ul_ri + 1);
  }
}

static int nr_srs_tpmi_estimation(const NR_PUSCH_Config_t *pusch_Config,
                                  const long transform_precoding,
                                  const uint8_t *channel_matrix,
                                  const uint8_t normalized_iq_representation,
                                  const uint16_t num_gnb_antenna_elements,
                                  const uint16_t num_ue_srs_ports,
                                  const uint16_t prg_size,
                                  const uint16_t num_prgs,
                                  const uint8_t ul_ri)
{
  if (ul_ri > 1) {
    LOG_D(NR_MAC, "TPMI computation for ul_ri %i is not implemented yet!\n", ul_ri);
    return 0;
  }

  uint8_t tpmi_sel = 0;
  const uint8_t nrOfLayers = ul_ri + 1;
  int16_t precoded_channel_matrix_re[num_prgs * num_gnb_antenna_elements];
  int16_t precoded_channel_matrix_im[num_prgs * num_gnb_antenna_elements];
  c16_t *channel_matrix16 = (c16_t *)channel_matrix;
  uint32_t max_precoded_signal_power = 0;
  int additional_max_tpmi = -1;
  char w;

  uint8_t max_tpmi = get_max_tpmi(pusch_Config, num_ue_srs_ports, &nrOfLayers, &additional_max_tpmi);
  uint8_t end_tpmi_loop = additional_max_tpmi > max_tpmi ? additional_max_tpmi : max_tpmi;

  //                      channel_matrix                          x   precoder_matrix
  // [ (gI=0,uI=0) (gI=0,uI=1) ... (gI=0,uI=num_ue_srs_ports-1) ] x   [uI=0]
  // [ (gI=1,uI=0) (gI=1,uI=1) ... (gI=1,uI=num_ue_srs_ports-1) ]     [uI=1]
  // [ (gI=2,uI=0) (gI=2,uI=1) ... (gI=2,uI=num_ue_srs_ports-1) ]     [uI=2]
  //                           ...                                     ...

  for (uint8_t tpmi = 0; tpmi <= end_tpmi_loop && end_tpmi_loop > 0; tpmi++) {
    if (tpmi > max_tpmi) {
      tpmi = end_tpmi_loop;
    }

    for (int pI = 0; pI < num_prgs; pI++) {
      for (int gI = 0; gI < num_gnb_antenna_elements; gI++) {
        uint16_t index_gI_pI = gI * num_prgs + pI;
        precoded_channel_matrix_re[index_gI_pI] = 0;
        precoded_channel_matrix_im[index_gI_pI] = 0;

        for (int uI = 0; uI < num_ue_srs_ports; uI++) {
          for (int layer_idx = 0; layer_idx < nrOfLayers; layer_idx++) {
            uint16_t index = uI * num_gnb_antenna_elements * num_prgs + index_gI_pI;
            get_precoder_matrix_coef(&w, ul_ri, num_ue_srs_ports, transform_precoding, tpmi, uI, layer_idx);
            c16_t h_times_w = nr_h_times_w(channel_matrix16[index], w);

            precoded_channel_matrix_re[index_gI_pI] += h_times_w.r;
            precoded_channel_matrix_im[index_gI_pI] += h_times_w.i;

#ifdef SRS_IND_DEBUG
            LOG_I(NR_MAC, "(pI %i, gI %i,  uI %i, layer_idx %i) w = %c, channel_matrix --> real %i, imag %i\n",
                  pI, gI, uI, layer_idx, w, channel_matrix16[index].r, channel_matrix16[index].i);
#endif
          }
        }

#ifdef SRS_IND_DEBUG
        LOG_I(NR_MAC, "(pI %i, gI %i) precoded_channel_coef --> real %i, imag %i\n",
              pI, gI, precoded_channel_matrix_re[index_gI_pI], precoded_channel_matrix_im[index_gI_pI]);
#endif
      }
    }

    uint32_t precoded_signal_power = calc_power_complex(precoded_channel_matrix_re,
                                                        precoded_channel_matrix_im,
                                                        num_prgs * num_gnb_antenna_elements);

#ifdef SRS_IND_DEBUG
    LOG_I(NR_MAC, "(tpmi %i) precoded_signal_power = %i\n", tpmi, precoded_signal_power);
#endif

    if (precoded_signal_power > max_precoded_signal_power) {
      max_precoded_signal_power = precoded_signal_power;
      tpmi_sel = tpmi;
    }
  }

  return tpmi_sel;
}

void handle_nr_srs_measurements(const module_id_t module_id,
                                const frame_t frame,
                                const slot_t slot,
                                nfapi_nr_srs_indication_pdu_t *srs_ind)
{
  gNB_MAC_INST *nrmac = RC.nrmac[module_id];
  LOG_D(NR_MAC, "(%d.%d) Received SRS indication for UE %04x\n", frame, slot, srs_ind->rnti);
  if (srs_ind->report_type == 0) {
    //SCF 222.10.04 Table 3-129 Report type = 0 means a null report, we can skip unpacking it
    return;
  }

  if (srs_ind->timing_advance_offset == 0xFFFF) {
    LOG_W(NR_MAC, "Invalid timing advance offset for RNTI %04x\n", srs_ind->rnti);
    return;
  }
  NR_SCHED_LOCK(&nrmac->sched_lock);

#ifdef SRS_IND_DEBUG
  LOG_I(NR_MAC, "frame = %i\n", frame);
  LOG_I(NR_MAC, "slot = %i\n", slot);
  LOG_I(NR_MAC, "srs_ind->rnti = %04x\n", srs_ind->rnti);
  LOG_I(NR_MAC, "srs_ind->timing_advance_offset = %i\n", srs_ind->timing_advance_offset);
  LOG_I(NR_MAC, "srs_ind->timing_advance_offset_nsec = %i\n", srs_ind->timing_advance_offset_nsec);
  LOG_I(NR_MAC, "srs_ind->srs_usage = %i\n", srs_ind->srs_usage);
  LOG_I(NR_MAC, "srs_ind->report_type = %i\n", srs_ind->report_type);
#endif

  NR_UE_info_t *UE = find_nr_UE(&RC.nrmac[module_id]->UE_info, srs_ind->rnti);
  if (!UE) {
    LOG_W(NR_MAC, "Could not find UE for RNTI %04x\n", srs_ind->rnti);
    NR_SCHED_UNLOCK(&nrmac->sched_lock);
    return;
  }

  gNB_MAC_INST *nr_mac = RC.nrmac[module_id];
  NR_mac_stats_t *stats = &UE->mac_stats;
  nfapi_srs_report_tlv_t *report_tlv = &srs_ind->report_tlv;

  switch (srs_ind->srs_usage) {
    case NR_SRS_ResourceSet__usage_beamManagement: {
      nfapi_nr_srs_beamforming_report_t nr_srs_bf_report;
      unpack_nr_srs_beamforming_report(report_tlv->value,
                                       report_tlv->length,
                                       &nr_srs_bf_report,
                                       sizeof(nfapi_nr_srs_beamforming_report_t));

      if (nr_srs_bf_report.wide_band_snr == 0xFF) {
        LOG_W(NR_MAC, "Invalid wide_band_snr for RNTI %04x\n", srs_ind->rnti);
        NR_SCHED_UNLOCK(&nrmac->sched_lock);
        return;
      }

      int wide_band_snr_dB = (nr_srs_bf_report.wide_band_snr >> 1) - 64;

#ifdef SRS_IND_DEBUG
      LOG_I(NR_MAC, "nr_srs_bf_report.prg_size = %i\n", nr_srs_bf_report.prg_size);
      LOG_I(NR_MAC, "nr_srs_bf_report.num_symbols = %i\n", nr_srs_bf_report.num_symbols);
      LOG_I(NR_MAC, "nr_srs_bf_report.wide_band_snr = %i (%i dB)\n", nr_srs_bf_report.wide_band_snr, wide_band_snr_dB);
      LOG_I(NR_MAC, "nr_srs_bf_report.num_reported_symbols = %i\n", nr_srs_bf_report.num_reported_symbols);
      LOG_I(NR_MAC, "nr_srs_bf_report.reported_symbol_list[0].num_prgs = %i\n", nr_srs_bf_report.reported_symbol_list[0].num_prgs);
      for (int prg_idx = 0; prg_idx < nr_srs_bf_report.reported_symbol_list[0].num_prgs; prg_idx++) {
        LOG_I(NR_MAC,
              "nr_srs_bf_report.reported_symbol_list[0].prg_list[%3i].rb_snr = %i (%i dB)\n",
              prg_idx,
              nr_srs_bf_report.reported_symbol_list[0].prg_list[prg_idx].rb_snr,
              (nr_srs_bf_report.reported_symbol_list[0].prg_list[prg_idx].rb_snr >> 1) - 64);
      }
#endif

      sprintf(stats->srs_stats, "UL-SNR %i dB", wide_band_snr_dB);

      const int ul_prbblack_SNR_threshold = nr_mac->ul_prbblack_SNR_threshold;
      uint16_t *ulprbbl = nr_mac->ulprbbl;

      uint16_t num_rbs = nr_srs_bf_report.prg_size * nr_srs_bf_report.reported_symbol_list[0].num_prgs;
      memset(ulprbbl, 0, num_rbs * sizeof(uint16_t));
      for (int rb = 0; rb < num_rbs; rb++) {
        int snr = (nr_srs_bf_report.reported_symbol_list[0].prg_list[rb / nr_srs_bf_report.prg_size].rb_snr >> 1) - 64;
        if (snr < wide_band_snr_dB - ul_prbblack_SNR_threshold) {
          ulprbbl[rb] = 0x3FFF; // all symbols taken
        }
        LOG_D(NR_MAC, "ulprbbl[%3i] = 0x%x\n", rb, ulprbbl[rb]);
      }

      break;
    }

    case NR_SRS_ResourceSet__usage_codebook: {
      nfapi_nr_srs_normalized_channel_iq_matrix_t nr_srs_channel_iq_matrix;
      unpack_nr_srs_normalized_channel_iq_matrix(report_tlv->value,
                                                 report_tlv->length,
                                                 &nr_srs_channel_iq_matrix,
                                                 sizeof(nfapi_nr_srs_normalized_channel_iq_matrix_t));

#ifdef SRS_IND_DEBUG
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.normalized_iq_representation = %i\n", nr_srs_channel_iq_matrix.normalized_iq_representation);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.num_gnb_antenna_elements = %i\n", nr_srs_channel_iq_matrix.num_gnb_antenna_elements);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.num_ue_srs_ports = %i\n", nr_srs_channel_iq_matrix.num_ue_srs_ports);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.prg_size = %i\n", nr_srs_channel_iq_matrix.prg_size);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.num_prgs = %i\n", nr_srs_channel_iq_matrix.num_prgs);
      c16_t *channel_matrix16 = (c16_t *)nr_srs_channel_iq_matrix.channel_matrix;
      c8_t *channel_matrix8 = (c8_t *)nr_srs_channel_iq_matrix.channel_matrix;
      for (int uI = 0; uI < nr_srs_channel_iq_matrix.num_ue_srs_ports; uI++) {
        for (int gI = 0; gI < nr_srs_channel_iq_matrix.num_gnb_antenna_elements; gI++) {
          for (int pI = 0; pI < nr_srs_channel_iq_matrix.num_prgs; pI++) {
            uint16_t index = uI * nr_srs_channel_iq_matrix.num_gnb_antenna_elements * nr_srs_channel_iq_matrix.num_prgs + gI * nr_srs_channel_iq_matrix.num_prgs + pI;
            LOG_I(NR_MAC,
                  "(uI %i, gI %i, pI %i) channel_matrix --> real %i, imag %i\n",
                  uI,
                  gI,
                  pI,
                  nr_srs_channel_iq_matrix.normalized_iq_representation == 0 ? channel_matrix8[index].r : channel_matrix16[index].r,
                  nr_srs_channel_iq_matrix.normalized_iq_representation == 0 ? channel_matrix8[index].i : channel_matrix16[index].i);
          }
        }
      }
#endif

      NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
      NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;
      sched_ctrl->srs_feedback.sri = NR_SRS_SRI_0;

      start_meas(&nr_mac->nr_srs_ri_computation_timer);
      nr_srs_ri_computation(&nr_srs_channel_iq_matrix, current_BWP, &sched_ctrl->srs_feedback.ul_ri);
      stop_meas(&nr_mac->nr_srs_ri_computation_timer);

      start_meas(&nr_mac->nr_srs_tpmi_computation_timer);
      sched_ctrl->srs_feedback.tpmi = nr_srs_tpmi_estimation(current_BWP->pusch_Config,
                                                             current_BWP->transform_precoding,
                                                             nr_srs_channel_iq_matrix.channel_matrix,
                                                             nr_srs_channel_iq_matrix.normalized_iq_representation,
                                                             nr_srs_channel_iq_matrix.num_gnb_antenna_elements,
                                                             nr_srs_channel_iq_matrix.num_ue_srs_ports,
                                                             nr_srs_channel_iq_matrix.prg_size,
                                                             nr_srs_channel_iq_matrix.num_prgs,
                                                             sched_ctrl->srs_feedback.ul_ri);
      stop_meas(&nr_mac->nr_srs_tpmi_computation_timer);

      sprintf(stats->srs_stats, "UL-RI %d, TPMI %d", sched_ctrl->srs_feedback.ul_ri + 1, sched_ctrl->srs_feedback.tpmi);

      break;
    }

    case NR_SRS_ResourceSet__usage_nonCodebook:
    case NR_SRS_ResourceSet__usage_antennaSwitching:
      LOG_W(NR_MAC, "MAC procedures for this SRS usage are not implemented yet!\n");
      break;

    default:
      AssertFatal(1 == 0, "Invalid SRS usage\n");
  }
  NR_SCHED_UNLOCK(&nrmac->sched_lock);
}

static bool nr_UE_is_to_be_scheduled(const frame_structure_t *fs,
                                     NR_UE_info_t *UE,
                                     frame_t frame,
                                     slot_t slot,
                                     uint32_t ulsch_max_frame_inactivity)
{
  const int n = fs->numb_slots_frame;
  const int now = frame * n + slot;

  const NR_UE_sched_ctrl_t *sched_ctrl =&UE->UE_sched_ctrl;
  /**
   * Force the default transmission in a full slot as early
   * as possible in the UL portion of TDD period (last_ul_slot) */
  int num_slots_per_period = fs->numb_slots_period;
  int last_ul_slot = fs->frame_type == TDD ? get_first_ul_slot(fs, false) : sched_ctrl->last_ul_slot;
  const int last_ul_sched = sched_ctrl->last_ul_frame * n + last_ul_slot;
  const int diff = (now - last_ul_sched + 1024 * n) % (1024 * n);
  /* UE is to be scheduled if
   * (1) we think the UE has more bytes awaiting than what we scheduled
   * (2) there is a scheduling request
   * (3) or we did not schedule it in more than 10 frames */
  const bool has_data = sched_ctrl->estimated_ul_buffer > sched_ctrl->sched_ul_bytes;
  const bool high_inactivity = diff >= (ulsch_max_frame_inactivity > 0 ? ulsch_max_frame_inactivity * n : num_slots_per_period);
  LOG_D(NR_MAC,
        "%4d.%2d UL inactivity %d slots has_data %d SR %d\n",
        frame,
        slot,
        diff,
        has_data,
        sched_ctrl->SR);
  return has_data || sched_ctrl->SR || high_inactivity;
}

static void update_ul_ue_R_Qm(int mcs, int mcs_table, const NR_PUSCH_Config_t *pusch_Config, uint16_t *R, uint8_t *Qm)
{
  *R = nr_get_code_rate_ul(mcs, mcs_table);
  *Qm = nr_get_Qm_ul(mcs, mcs_table);

  if (pusch_Config && pusch_Config->tp_pi2BPSK && ((mcs_table == 3 && mcs < 2) || (mcs_table == 4 && mcs < 6))) {
    *R >>= 1;
    *Qm <<= 1;
  }
}

/* Apply a retransmission allocation: build NR_sched_pusch_t from stored HARQ info,
 * handle TDA mismatch, call post_process_ulsch, mark VRB map.
 * Returns rbSize on success, -1 on failure (TDA mismatch with no TBS match). */
static int apply_ul_retransmission(gNB_MAC_INST *nrmac,
                                   post_process_pusch_t *pp_pusch,
                                   const nr_ul_candidate_t *cand,
                                   const nr_ul_alloc_t *alloc,
                                   const NR_ServingCellConfigCommon_t *scc,
                                   int tda,
                                   const NR_tda_info_t *tda_info,
                                   uint16_t *rballoc_mask,
                                   bwp_info_t bi,
                                   int sched_frame, int sched_slot)
{
  NR_UE_info_t *UE = cand->UE;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;

  int harq_pid = cand->retx_harq_pid;
  const NR_sched_pusch_t *retInfo = &sched_ctrl->ul_harq_processes[harq_pid].sched_pusch;
  NR_sched_pusch_t new_sched = *retInfo;
  const uint8_t nrOfLayers = retInfo->nrOfLayers;

  new_sched.frame = sched_frame;
  new_sched.slot = sched_slot;
  new_sched.bwp_info = bi;
  DevAssert(new_sched.ul_harq_pid == harq_pid);

  bool reuse_old_tda = retInfo->time_domain_allocation == tda;
  if (reuse_old_tda && nrOfLayers == retInfo->nrOfLayers) {
    new_sched.rbStart = alloc->rbStart;
    LOG_D(NR_MAC, "Retransmission keeping TDA %d and TBS %d\n", tda, retInfo->tb_size);
  } else {
    NR_pusch_dmrs_t dmrs_info = get_ul_dmrs_params(scc, current_BWP, tda_info, nrOfLayers);
    uint32_t new_tbs;
    uint16_t new_rbSize;
    bool success = nr_find_nb_rb(retInfo->Qm, retInfo->R,
                                 current_BWP->transform_precoding,
                                 nrOfLayers, tda_info->nrOfSymbols,
                                 dmrs_info.N_PRB_DMRS * dmrs_info.num_dmrs_symb,
                                 retInfo->tb_size, 1, alloc->rbSize,
                                 &new_tbs, &new_rbSize);
    if (!success || new_tbs != retInfo->tb_size) {
      LOG_D(NR_MAC, "[UE %04x] retx TDA change failed: new TBS %d != old TBS %d\n",
            UE->rnti, new_tbs, retInfo->tb_size);
      return -1;
    }
    new_sched.rbSize = new_rbSize;
    new_sched.rbStart = alloc->rbStart;
    new_sched.tb_size = new_tbs;
    new_sched.time_domain_allocation = tda;
    new_sched.tda_info = *tda_info;
    new_sched.dmrs_info = dmrs_info;
    LOG_D(NR_MAC, "Retransmission with TDA %d->%d and TBS %d -> %d\n",
          retInfo->time_domain_allocation, tda, retInfo->tb_size, new_tbs);
  }

  DevAssert(new_sched.time_domain_allocation == tda);
  post_process_ulsch(nrmac, pp_pusch, UE, &new_sched);

  const uint16_t retx_slbitmap = SL_to_bitmap(new_sched.tda_info.startSymbolIndex, new_sched.tda_info.nrOfSymbols);
  for (int rb = 0; rb < new_sched.rbSize; rb++)
    rballoc_mask[new_sched.rbStart + bi.bwpStart + rb] |= retx_slbitmap;

  LOG_D(NR_MAC, "%4d.%2d Allocate UL retransmission RNTI %04x sched %4d.%2d (%d RBs)\n",
        pp_pusch->frame, pp_pusch->slot, UE->rnti, new_sched.frame, new_sched.slot, new_sched.rbSize);

  return new_sched.rbSize;
}

/* Apply a new-transmission allocation: build NR_sched_pusch_t from policy output,
 * compute TBS and PHR, call post_process_ulsch, mark VRB map.
 * Returns rbSize. */
static int apply_ul_new_transmission(gNB_MAC_INST *nrmac,
                                     post_process_pusch_t *pp_pusch,
                                     const nr_ul_candidate_t *cand,
                                     const nr_ul_alloc_t *alloc,
                                     const NR_ServingCellConfigCommon_t *scc,
                                     int tda,
                                     const NR_tda_info_t *tda_info,
                                     uint16_t *rballoc_mask,
                                     uint16_t slbitmap,
                                     bwp_info_t bi,
                                     int sched_frame, int sched_slot,
                                     int CCEIndex)
{
  NR_UE_info_t *UE = cand->UE;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;

  int nrOfLayers = get_ul_nrOfLayers(sched_ctrl, current_BWP->dci_format);
  NR_sched_pusch_t sched = {
    .frame = sched_frame,
    .slot = sched_slot,
    .rbStart = alloc->rbStart,
    .mcs = alloc->mcs,
    .ul_harq_pid = -1,
    .nrOfLayers = nrOfLayers,
    .time_domain_allocation = tda,
    .tda_info = *tda_info,
    .dmrs_info = get_ul_dmrs_params(scc, current_BWP, tda_info, nrOfLayers),
    .bwp_info = bi,
  };

  update_ul_ue_R_Qm(sched.mcs, current_BWP->mcs_table, current_BWP->pusch_Config, &sched.R, &sched.Qm);

  sched.rbSize = alloc->rbSize;
  sched.tb_size = nr_compute_tbs(sched.Qm, sched.R, sched.rbSize,
                                  sched.tda_info.nrOfSymbols,
                                  sched.dmrs_info.N_PRB_DMRS * sched.dmrs_info.num_dmrs_symb,
                                  0, 0, sched.nrOfLayers) >> 3;

  long *deltaMCS = current_BWP->pusch_Config ? current_BWP->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
  sched.phr_txpower_calc = compute_ph_factor(current_BWP->scs,
                                              sched.tb_size << 3,
                                              sched.rbSize,
                                              sched.nrOfLayers,
                                              sched.tda_info.nrOfSymbols,
                                              sched.dmrs_info.N_PRB_DMRS * sched.dmrs_info.num_dmrs_symb,
                                              deltaMCS, false);

  LOG_D(NR_MAC,
        "rbSize %d, TBS %d, est buf %d, sched_ul %d, B %d, CCE %d, num_dmrs_symb %d, N_PRB_DMRS %d\n",
        sched.rbSize, sched.tb_size,
        sched_ctrl->estimated_ul_buffer, sched_ctrl->sched_ul_bytes,
        cmax(sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes, 0),
        CCEIndex, sched.dmrs_info.num_dmrs_symb, sched.dmrs_info.N_PRB_DMRS);

  post_process_ulsch(nrmac, pp_pusch, UE, &sched);

  for (int rb = 0; rb < sched.rbSize; rb++)
    rballoc_mask[sched.rbStart + bi.bwpStart + rb] |= slbitmap;

  return sched.rbSize;
}

static int nr_ul_schedule(gNB_MAC_INST *nrmac,
                          post_process_pusch_t *pp_pusch,
                          int tda,
                          const NR_tda_info_t *tda_info,
                          NR_UE_info_t *UE_list[],
                          int max_num_ue,
                          int num_beams,
                          int n_rb_sched[num_beams])
{
  const int CC_id = 0;
  int frame = pp_pusch->frame;
  int slot = pp_pusch->slot;
  NR_ServingCellConfigCommon_t *scc = nrmac->common_channels[CC_id].ServingCellConfigCommon;
  int slots_per_frame = nrmac->frame_structure.numb_slots_frame;
  DevAssert(tda_info->valid_tda);
  const int k2 = tda_info->k2 + get_NTN_Koffset(scc);
  const int sched_frame = (frame + (slot + k2) / slots_per_frame) % MAX_FRAME_NUMBER;
  const int sched_slot = (slot + k2) % slots_per_frame;
  DevAssert(is_ul_slot(sched_slot, &nrmac->frame_structure));

  const int min_rb = nrmac->min_grant_prb;

  /* Step 1: Collect candidates */
  nr_ul_candidate_t candidates[MAX_MOBILES_PER_GNB] = {0};
  int n_cand = collect_ul_candidates(nrmac, UE_list, candidates, MAX_MOBILES_PER_GNB,
                                     frame, slot, sched_frame, sched_slot, tda, tda_info);
  if (n_cand == 0)
    return 0;

  /* Step 2: Beam allocation via function pointer (allocates both DCI and sched beams) */
  n_cand = nrmac->ul_beam_alloc(&nrmac->beam_info, candidates, n_cand,
                                 frame, slot, sched_frame, sched_slot, slots_per_frame);
  if (n_cand == 0)
    return 0;

  /* Step 3: Per-beam policy scheduling */
  nr_ul_alloc_t allocs[MAX_MOBILES_PER_GNB] = {0};
  const uint16_t slbitmap = SL_to_bitmap(tda_info->startSymbolIndex, tda_info->nrOfSymbols);

  /* Check if any UE has a pending RA process — if so, the scheduling policy
   * should leave room for msg3 allocation (8 RBs).  We temporarily mark RBs
   * as used before calling the policy and restore them afterwards so that
   * nr_get_Msg3alloc() can find free space later. */
  const bool ra_pending = nrmac->UE_info.access_ue_list[0] != NULL;
  const int ra_msg3_rbs = 8;

  for (int b = 0; b < num_beams; b++) {
    /* Filter candidates for this beam */
    nr_ul_candidate_t beam_cands[MAX_MOBILES_PER_GNB];
    int beam_map[MAX_MOBILES_PER_GNB]; /* mapping back to global index */
    int n_beam_cand = 0;

    for (int i = 0; i < n_cand; i++) {
      if (candidates[i].alloc_beam_idx == b) {
        beam_cands[n_beam_cand] = candidates[i];
        beam_map[n_beam_cand] = i;
        n_beam_cand++;
      }
    }

    if (n_beam_cand == 0)
      continue;

    const int index = ul_buffer_index(sched_frame, sched_slot, slots_per_frame, nrmac->vrb_map_UL_size);
    uint16_t *vrb_map_UL = &nrmac->common_channels[CC_id].vrb_map_UL[b][index * MAX_BWP_SIZE];

    /* RA priority: temporarily reserve RBs at the end of the BWP so the
     * scheduling policy does not fill the entire band */
    int ra_reserve_start = -1;
    uint16_t ra_saved_vrb[8];
    if (ra_pending) {
      int bwp_s = beam_cands[0].bwp_start;
      int bwp_sz = beam_cands[0].bwp_size;
      /* Search backwards from end of BWP for ra_msg3_rbs contiguous free RBs */
      int count = 0;
      for (int rb = bwp_sz - 1; rb >= 0; rb--) {
        if (!(vrb_map_UL[rb + bwp_s] & slbitmap)) {
          count++;
          if (count == ra_msg3_rbs) {
            ra_reserve_start = rb + bwp_s;
            break;
          }
        } else {
          count = 0;
        }
      }
      if (ra_reserve_start >= 0) {
        for (int r = 0; r < ra_msg3_rbs; r++) {
          ra_saved_vrb[r] = vrb_map_UL[ra_reserve_start + r];
          vrb_map_UL[ra_reserve_start + r] |= slbitmap;
        }
        LOG_D(NR_MAC, "RA pending: reserved %d RBs at %d for msg3 in %d.%d\n",
              ra_msg3_rbs, ra_reserve_start, sched_frame, sched_slot);
      }
    }

    nr_ul_sched_params_t params = {
      .frame = sched_frame,
      .slot = sched_slot,
      .beam_idx = b,
      .max_num_ue = max_num_ue,
      .slbitmap = slbitmap,
      .vrb_map_UL = vrb_map_UL,
      .n_rb_avail = n_rb_sched[b],
      .min_rb = min_rb,
      .min_mcs = nrmac->ul_bler.min_mcs,
      .bler_lower = nrmac->ul_bler.lower,
      .bler_upper = nrmac->ul_bler.upper,
      .tda = tda,
      .tda_info = tda_info,
      .scc = scc,
    };

    nr_ul_alloc_t beam_allocs[MAX_MOBILES_PER_GNB] = {0};
    nrmac->ul_sched_policy(&params, beam_cands, beam_allocs, n_beam_cand);

    /* Restore RA-reserved RBs so nr_get_Msg3alloc() can use them */
    if (ra_reserve_start >= 0) {
      for (int r = 0; r < ra_msg3_rbs; r++)
        vrb_map_UL[ra_reserve_start + r] = ra_saved_vrb[r];
    }

    /* Map back to global allocs array */
    for (int j = 0; j < n_beam_cand; j++)
      allocs[beam_map[j]] = beam_allocs[j];
  }

  /* Step 4: Apply allocations — CCE, build NR_sched_pusch_t, post_process_ulsch, mark VRB map */
  int remainUEs[num_beams];
  for (int i = 0; i < num_beams; i++)
    remainUEs[i] = max_num_ue;
  int num_ue_sched = 0;

  for (int i = 0; i < n_cand; i++) {
    if (!allocs[i].scheduled)
      continue;

    nr_ul_candidate_t *cand = &candidates[i];
    NR_UE_info_t *UE = cand->UE;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    int beam_idx = cand->alloc_beam_idx;

    if (remainUEs[beam_idx] == 0 || n_rb_sched[beam_idx] < min_rb) {
      reset_beam_status(&nrmac->beam_info, frame, slot, cand->beam_index, slots_per_frame, cand->dci_beam_new);
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, cand->beam_index, slots_per_frame, cand->sched_beam_new);
      continue;
    }

    /* Find a free CCE */
    int CCEIndex = get_cce_index(nrmac, CC_id, slot, UE->rnti,
                                 &sched_ctrl->aggregation_level,
                                 cand->dci_beam_idx,
                                 sched_ctrl->search_space,
                                 sched_ctrl->coreset,
                                 &sched_ctrl->sched_pdcch,
                                 sched_ctrl->pdcch_cl_adjust);
    if (CCEIndex < 0) {
      sched_ctrl->ul_cce_fail++;
      reset_beam_status(&nrmac->beam_info, frame, slot, cand->beam_index, slots_per_frame, cand->dci_beam_new);
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, cand->beam_index, slots_per_frame, cand->sched_beam_new);
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] no free CCE for UL DCI\n", UE->rnti, frame, slot);
      continue;
    }

    sched_ctrl->cce_index = CCEIndex;
    fill_pdcch_vrb_map(nrmac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, cand->dci_beam_idx);

    const int buf_index = ul_buffer_index(sched_frame, sched_slot, slots_per_frame, nrmac->vrb_map_UL_size);
    uint16_t *rballoc_mask = &nrmac->common_channels[CC_id].vrb_map_UL[beam_idx][buf_index * MAX_BWP_SIZE];
    bwp_info_t bi = get_pusch_bwp_start_size(UE);

    int rbSize_used;
    if (cand->is_retx) {
      rbSize_used = apply_ul_retransmission(nrmac, pp_pusch, cand, &allocs[i],
                                            scc, tda, tda_info, rballoc_mask, bi,
                                            sched_frame, sched_slot);
      if (rbSize_used < 0) {
        reset_beam_status(&nrmac->beam_info, frame, slot, cand->beam_index, slots_per_frame, cand->dci_beam_new);
        reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, cand->beam_index, slots_per_frame, cand->sched_beam_new);
        continue;
      }
    } else {
      rbSize_used = apply_ul_new_transmission(nrmac, pp_pusch, cand, &allocs[i],
                                              scc, tda, tda_info, rballoc_mask, slbitmap, bi,
                                              sched_frame, sched_slot, CCEIndex);
    }

    n_rb_sched[beam_idx] -= rbSize_used;
    remainUEs[beam_idx]--;
    num_ue_sched++;
  }

  return num_ue_sched;
}

nfapi_nr_pusch_pdu_t *prepare_pusch_pdu(nfapi_nr_ul_tti_request_t *future_ul_tti_req,
                                        const NR_UE_info_t *UE,
                                        const NR_ServingCellConfigCommon_t *scc,
                                        const NR_sched_pusch_t *sched_pusch,
                                        int transform_precoding,
                                        int harq_id,
                                        int harq_round,
                                        int fh,
                                        int rnti,
                                        nr_beam_mode_t beam_mode)
{
  nfapi_nr_pusch_pdu_t *pusch_pdu = &future_ul_tti_req->pdus_list[future_ul_tti_req->n_pdus].pusch_pdu;
  memset(pusch_pdu, 0, sizeof(nfapi_nr_pusch_pdu_t));
  const NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;

  pusch_pdu->pdu_bit_map = PUSCH_PDU_BITMAP_PUSCH_DATA;
  pusch_pdu->rnti = rnti;
  pusch_pdu->handle = 0; //not yet used
  pusch_pdu->bwp_size = sched_pusch->bwp_info.bwpSize;
  pusch_pdu->bwp_start = sched_pusch->bwp_info.bwpStart;
  pusch_pdu->subcarrier_spacing = ul_bwp->scs;
  pusch_pdu->cyclic_prefix = 0;
  pusch_pdu->target_code_rate = sched_pusch->R;
  pusch_pdu->qam_mod_order = sched_pusch->Qm;
  pusch_pdu->mcs_index = sched_pusch->mcs;
  pusch_pdu->mcs_table = ul_bwp->mcs_table;
  pusch_pdu->transform_precoding = transform_precoding;
  if (ul_bwp->pusch_Config && ul_bwp->pusch_Config->dataScramblingIdentityPUSCH)
    pusch_pdu->data_scrambling_id = *ul_bwp->pusch_Config->dataScramblingIdentityPUSCH;
  else
    pusch_pdu->data_scrambling_id = *scc->physCellId;
  pusch_pdu->nrOfLayers = sched_pusch->nrOfLayers;
  // DMRS
  pusch_pdu->num_dmrs_cdm_grps_no_data = sched_pusch->dmrs_info.num_dmrs_cdm_grps_no_data;
  pusch_pdu->dmrs_ports = ((1 << sched_pusch->nrOfLayers) - 1);
  pusch_pdu->ul_dmrs_symb_pos = sched_pusch->dmrs_info.ul_dmrs_symb_pos;
  pusch_pdu->dmrs_config_type = sched_pusch->dmrs_info.dmrs_config_type;
  pusch_pdu->scid = sched_pusch->dmrs_info.scid; // DMRS sequence initialization [TS38.211, sec 6.4.1.1.1]
  pusch_pdu->pusch_identity = sched_pusch->dmrs_info.pusch_identity;
  pusch_pdu->ul_dmrs_scrambling_id = sched_pusch->dmrs_info.dmrs_scrambling_id;
  /* Allocation in frequency domain */
  pusch_pdu->resource_alloc = 1; //type 1
  pusch_pdu->rb_start = sched_pusch->rbStart;
  pusch_pdu->rb_size = sched_pusch->rbSize;
  pusch_pdu->vrb_to_prb_mapping = 0;
  pusch_pdu->frequency_hopping = fh;
  /* Resource Allocation in time domain */
  pusch_pdu->start_symbol_index = sched_pusch->tda_info.startSymbolIndex;
  pusch_pdu->nr_of_symbols = sched_pusch->tda_info.nrOfSymbols;
  /* PUSCH PDU */
  pusch_pdu->pusch_data.rv_index = nr_get_rv(harq_round % 4);
  pusch_pdu->pusch_data.harq_process_id = harq_id;
  pusch_pdu->pusch_data.new_data_indicator = (harq_round == 0) ? 1 : 0; // not NDI but indicator for new transmission
  pusch_pdu->pusch_data.tb_size = sched_pusch->tb_size;
  pusch_pdu->pusch_data.num_cb = 0; // CBG not supported
  // Beamforming
  pusch_pdu->beamforming.num_prgs = 1;
  pusch_pdu->beamforming.prg_size = pusch_pdu->bwp_size;
  pusch_pdu->beamforming.dig_bf_interface = 1;
  pusch_pdu->beamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx =
      convert_to_fapi_beam(UE->UE_beam_index, beam_mode);
  /* TRANSFORM PRECODING --------------------------------------------------------*/
  if (pusch_pdu->transform_precoding == NR_PUSCH_Config__transformPrecoder_enabled) {
    // U as specified in section 6.4.1.1.1.2 in 38.211, if sequence hopping and group hopping are disabled
    pusch_pdu->dfts_ofdm.low_papr_group_number = sched_pusch->dmrs_info.pusch_identity % 30;
    pusch_pdu->dfts_ofdm.low_papr_sequence_number = sched_pusch->dmrs_info.low_papr_sequence_number;
  }


  pusch_pdu->maintenance_parms_v3.ldpcBaseGraph = get_BG(sched_pusch->tb_size << 3, sched_pusch->R);
  pusch_pdu->maintenance_parms_v3.tbSizeLbrmBytes = 0;
  if (UE->sc_info.rateMatching_PUSCH) {
    // TBS_LBRM according to section 5.4.2.1 of 38.212
    long *maxMIMO_Layers = UE->sc_info.maxMIMO_Layers_PUSCH;
    if (!maxMIMO_Layers && ul_bwp && ul_bwp->pusch_Config)
      maxMIMO_Layers = ul_bwp->pusch_Config->maxRank;
    long lbrm_layers = maxMIMO_Layers ? *maxMIMO_Layers : ue_supported_ul_layers(UE->capability);
    AssertFatal (maxMIMO_Layers != NULL,"Option with max MIMO layers not configured is not supported\n");
    pusch_pdu->maintenance_parms_v3.tbSizeLbrmBytes = nr_compute_tbslbrm(ul_bwp->mcs_table, UE->sc_info.ul_bw_tbslbrm, lbrm_layers);
  }
  /* PUSCH PTRS */
  if (sched_pusch->dmrs_info.ptrsConfig) {
    bool valid_ptrs_setup = false;
    pusch_pdu->pusch_ptrs.ptrs_ports_list = calloc_or_fail(2, sizeof(nfapi_nr_ptrs_ports_t));
    valid_ptrs_setup = set_ul_ptrs_values(sched_pusch->dmrs_info.ptrsConfig,
                                          pusch_pdu->rb_size,
                                          pusch_pdu->mcs_index,
                                          pusch_pdu->mcs_table,
                                          &pusch_pdu->pusch_ptrs.ptrs_freq_density,
                                          &pusch_pdu->pusch_ptrs.ptrs_time_density,
                                          &pusch_pdu->pusch_ptrs.ptrs_ports_list->ptrs_re_offset,
                                          &pusch_pdu->pusch_ptrs.num_ptrs_ports,
                                          &pusch_pdu->pusch_ptrs.ul_ptrs_power,
                                          pusch_pdu->nr_of_symbols);
    if (valid_ptrs_setup == true)
      pusch_pdu->pdu_bit_map |= PUSCH_PDU_BITMAP_PUSCH_PTRS; // enable PUSCH PTRS
  } else
    pusch_pdu->pdu_bit_map &= ~PUSCH_PDU_BITMAP_PUSCH_PTRS; // disable PUSCH PTRS
  return pusch_pdu;
}

void post_process_ulsch(gNB_MAC_INST *nr_mac, post_process_pusch_t *pusch, NR_UE_info_t *UE, NR_sched_pusch_t *sched_pusch)
{
  frame_t frame = pusch->frame;
  slot_t slot = pusch->slot;

  NR_ServingCellConfigCommon_t *scc = nr_mac->common_channels[0].ServingCellConfigCommon;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;

  /* the UE now has the grant for the request */
  sched_ctrl->SR = false;

  int8_t harq_id = sched_pusch->ul_harq_pid;
  if (harq_id < 0) {
    /* PP has not selected a specific HARQ Process, get a new one */
    harq_id = sched_ctrl->available_ul_harq.head;
    AssertFatal(harq_id >= 0, "no free HARQ process available for UE %04x\n", UE->rnti);
    remove_front_nr_list(&sched_ctrl->available_ul_harq);
    sched_pusch->ul_harq_pid = harq_id;
  } else {
    /* PP selected a specific HARQ process. Check whether it will be a new
     * transmission or a retransmission, and remove from the corresponding
     * list */
    if (sched_ctrl->ul_harq_processes[harq_id].round == 0)
      remove_nr_list(&sched_ctrl->available_ul_harq, harq_id);
    else
      remove_nr_list(&sched_ctrl->retrans_ul_harq, harq_id);
  }
  NR_UE_ul_harq_t *cur_harq = &sched_ctrl->ul_harq_processes[harq_id];
  DevAssert(!cur_harq->is_waiting);
  if (nr_mac->radio_config.disable_harq) {
    finish_nr_ul_harq(sched_ctrl, harq_id);
  } else {
    add_tail_nr_list(&sched_ctrl->feedback_ul_harq, harq_id);
    cur_harq->feedback_slot = sched_pusch->slot;
    cur_harq->is_waiting = true;
  }

  /* Statistics */
  AssertFatal(cur_harq->round < nr_mac->ul_bler.harq_round_max, "Indexing ulsch_rounds[%d] is out of bounds\n", cur_harq->round);
  UE->mac_stats.ul.rounds[cur_harq->round]++;
  /* Save information on MCS, TBS etc for the current initial transmission
   * so we have access to it when retransmitting */
  cur_harq->sched_pusch = *sched_pusch;
  if (cur_harq->round == 0) {
    UE->mac_stats.ulsch_total_bytes_scheduled += sched_pusch->tb_size;
    sched_ctrl->sched_ul_bytes += sched_pusch->tb_size;
    UE->mac_stats.ul.total_rbs += sched_pusch->rbSize;

  } else {
    UE->mac_stats.ul.total_rbs_retx += sched_pusch->rbSize;
  }
  UE->mac_stats.ul.current_bytes = sched_pusch->tb_size;
  UE->mac_stats.ul.current_rbs = sched_pusch->rbSize;
  nr_mac->mac_stats.ul.used_prb_aggregate += sched_pusch->rbSize;
  sched_ctrl->last_ul_frame = sched_pusch->frame;
  sched_ctrl->last_ul_slot = sched_pusch->slot;

  LOG_D(NR_MAC,
        "ULSCH/PUSCH: %4d.%2d RNTI %04x UL sched %4d.%2d DCI L %d start %2d RBS %3d TDA %2d dmrs_pos %x MCS "
        "Table %2d MCS %2d nrOfLayers %2d num_dmrs_cdm_grps_no_data %2d TBS %4d HARQ PID %2d round %d RV %d NDI %d est %6d sched "
        "%6d est BSR %6d TPC %d\n",
        frame,
        slot,
        UE->rnti,
        sched_pusch->frame,
        sched_pusch->slot,
        sched_ctrl->aggregation_level,
        sched_pusch->rbStart,
        sched_pusch->rbSize,
        sched_pusch->time_domain_allocation,
        sched_pusch->dmrs_info.ul_dmrs_symb_pos,
        current_BWP->mcs_table,
        sched_pusch->mcs,
        sched_pusch->nrOfLayers,
        sched_pusch->dmrs_info.num_dmrs_cdm_grps_no_data,
        sched_pusch->tb_size,
        harq_id,
        cur_harq->round,
        nr_get_rv(cur_harq->round % 4),
        cur_harq->ndi,
        sched_ctrl->estimated_ul_buffer,
        sched_ctrl->sched_ul_bytes,
        sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes,
        sched_ctrl->tpc0);

  T(T_GNB_MAC_UL, T_INT(UE->rnti), T_INT(frame), T_INT(slot), T_INT(sched_pusch->mcs), T_INT(sched_pusch->tb_size));

  /* PUSCH in a later slot, but corresponding DCI now! */
  const int index = ul_buffer_index(sched_pusch->frame,
                                    sched_pusch->slot,
                                    nr_mac->frame_structure.numb_slots_frame,
                                    nr_mac->UL_tti_req_ahead_size);
  nfapi_nr_ul_tti_request_t *req = &nr_mac->UL_tti_req_ahead[0][index];
  if (req->SFN != sched_pusch->frame || req->Slot != sched_pusch->slot)
    LOG_W(NR_MAC,
          "%d.%d future UL_tti_req's frame.slot %d.%d does not match PUSCH %d.%d\n",
          frame,
          slot,
          req->SFN,
          req->Slot,
          sched_pusch->frame,
          sched_pusch->slot);
  AssertFatal(req->n_pdus < sizeof(req->pdus_list) / sizeof(req->pdus_list[0]),
              "Invalid UL_tti_req_ahead->n_pdus %d\n",
              req->n_pdus);

  req->pdus_list[req->n_pdus].pdu_type = NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE;
  req->pdus_list[req->n_pdus].pdu_size = sizeof(nfapi_nr_pusch_pdu_t);
  nfapi_nr_pusch_pdu_t *pusch_pdu = prepare_pusch_pdu(req,
                                                      UE,
                                                      scc,
                                                      sched_pusch,
                                                      current_BWP->transform_precoding,
                                                      harq_id,
                                                      cur_harq->round,
                                                      current_BWP->pusch_Config && current_BWP->pusch_Config->frequencyHopping,
                                                      UE->rnti,
                                                      nr_mac->beam_info.beam_mode);
  req->n_pdus += 1;

  // Calculate the normalized tx_power for PHR
  long *deltaMCS = current_BWP->pusch_Config ? current_BWP->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
  int tbs_bits = pusch_pdu->pusch_data.tb_size << 3;
  sched_pusch->phr_txpower_calc = compute_ph_factor(current_BWP->scs,
                                                    tbs_bits,
                                                    sched_pusch->rbSize,
                                                    sched_pusch->nrOfLayers,
                                                    sched_pusch->tda_info.nrOfSymbols,
                                                    sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                                    deltaMCS,
                                                    false);

  /* a PDCCH PDU groups DCIs per BWP and CORESET. Save a pointer to each
   * allocated PDCCH so we can easily allocate UE's DCIs independent of any
   * CORESET order */
  NR_SearchSpace_t *ss = sched_ctrl->search_space;
  NR_ControlResourceSet_t *coreset = sched_ctrl->coreset;
  const int coresetid = coreset->controlResourceSetId;
  nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu = pusch->pdcch_pdu_coreset[coresetid];
  if (!pdcch_pdu) {
    nfapi_nr_ul_dci_request_pdus_t *ul_dci_request_pdu = &pusch->ul_dci_req->ul_dci_pdu_list[pusch->ul_dci_req->numPdus];
    memset(ul_dci_request_pdu, 0, sizeof(nfapi_nr_ul_dci_request_pdus_t));
    ul_dci_request_pdu->PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
    ul_dci_request_pdu->PDUSize = (uint8_t)(4 + sizeof(nfapi_nr_dl_tti_pdcch_pdu));
    pdcch_pdu = &ul_dci_request_pdu->pdcch_pdu.pdcch_pdu_rel15;
    pusch->ul_dci_req->numPdus += 1;
    nr_configure_pdcch(pdcch_pdu, coreset, &sched_ctrl->sched_pdcch);
    pusch->pdcch_pdu_coreset[coresetid] = pdcch_pdu;
  }

  LOG_D(NR_MAC, "Configuring ULDCI/PDCCH in %d.%d at CCE %d, rnti %04x\n", frame, slot, sched_ctrl->cce_index, UE->rnti);

  /* Fill PDCCH DL DCI PDU */
  nfapi_nr_dl_dci_pdu_t *dci_pdu = prepare_dci_pdu(pdcch_pdu,
                                                   scc,
                                                   ss,
                                                   coreset,
                                                   sched_ctrl->aggregation_level,
                                                   sched_ctrl->cce_index,
                                                   convert_to_fapi_beam(UE->UE_beam_index, nr_mac->beam_info.beam_mode),
                                                   UE->rnti);
  pdcch_pdu->numDlDci++;

  dci_pdu_rel15_t uldci_payload;
  memset(&uldci_payload, 0, sizeof(uldci_payload));
  if (current_BWP->dci_format == NR_UL_DCI_FORMAT_0_1)
    LOG_D(NR_MAC_DCI,
          "add ul dci harq %d for %d.%d %d.%d round %d\n",
          harq_id,
          frame,
          slot,
          sched_pusch->frame,
          sched_pusch->slot,
          sched_ctrl->ul_harq_processes[harq_id].round);

  // If nrOfLayers is the same as in srs_feedback, we use the best TPMI, i.e. the one in srs_feedback.
  // Otherwise, we use the valid TPMI that we saved in the first transmission.
  int *tpmi = NULL;
  if (pusch_pdu->nrOfLayers != (sched_ctrl->srs_feedback.ul_ri + 1))
    tpmi = &sched_pusch->tpmi;
  config_uldci(&UE->sc_info,
               pusch_pdu,
               &uldci_payload,
               &sched_ctrl->srs_feedback,
               tpmi,
               sched_pusch->time_domain_allocation,
               UE->UE_sched_ctrl.tpc0,
               cur_harq->ndi,
               current_BWP,
               ss->searchSpaceType->present);

  // Accumulate TPC command before resetting
  UE->UE_sched_ctrl.accumulated_tpc_db += tpc_to_db(UE->UE_sched_ctrl.tpc0);

  // Reset TPC to 0 dB to not request new gain multiple times before computing new value for SNR
  UE->UE_sched_ctrl.tpc0 = 1;

  fill_dci_pdu_rel15(&UE->sc_info,
                     &UE->current_DL_BWP,
                     current_BWP,
                     dci_pdu,
                     &uldci_payload,
                     current_BWP->dci_format,
                     TYPE_C_RNTI_,
                     ss,
                     coreset,
                     UE->pdsch_HARQ_ACK_Codebook,
                     nr_mac->cset0_bwp_size);
}

fsn_t fs_add_delta(const frame_structure_t *fs, uint32_t delta, fsn_t fsn)
{
  const int slots_frame = fs->numb_slots_frame;
  fsn_t res = {
    .f = (fsn.f + (fsn.s + delta) / slots_frame) % MAX_FRAME_NUMBER,
    .s = (fsn.s + delta) % slots_frame,
  };
  return res;
}
int fs_get_diff(const frame_structure_t *fs, fsn_t a, fsn_t b)
{
  const int slots_frame = fs->numb_slots_frame;
  int diff = (a.f * slots_frame + a.s) - (b.f * slots_frame + b.s);
  if (diff < -512 * slots_frame)
    diff += 1024 * slots_frame;
  else if (diff > 512 * slots_frame)
    diff -= 1024 * slots_frame;
  return diff;
}
fsn_t fs_get_max(const frame_structure_t *fs, fsn_t a, fsn_t b)
{
  if (fs_get_diff(fs, a, b) <= 0) {
    return b;
  } else {
    return a;
  }
}

static void nr_ulsch_preprocessor(gNB_MAC_INST *nr_mac, post_process_pusch_t *pp_pusch)
{
  int frame = pp_pusch->frame;
  int slot = pp_pusch->slot;

  NR_COMMON_channels_t *cc = nr_mac->common_channels;
  NR_ServingCellConfigCommon_t *scc = cc->ServingCellConfigCommon;
  AssertFatal(scc, "We need one serving cell config common\n");
  const frame_structure_t *fs = &nr_mac->frame_structure;

  // we assume the same K2 for all UEs
  const int koffset = get_NTN_Koffset(scc);
  const int min_rxtx = nr_mac->radio_config.minRXTXTIME + koffset;

  int num_beams = nr_mac->beam_info.beam_allocation ? nr_mac->beam_info.beams_per_period : 1;
  int bw = scc->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth;

  int average_agg_level = 4; // TODO find a better estimation
  int max_dci = bw / (average_agg_level * NR_NB_REG_PER_CCE);

  // FAPI cannot handle more than MAX_DCI_CORESET DCIs
  max_dci = min(max_dci, MAX_DCI_CORESET);

  fsn_t current = {frame, slot};
  fsn_t min_next = fs_add_delta(fs, min_rxtx, current);
  /* if it's the last DL slot, we should try all TDAs to make sure that the
   * scheduler can reach e.g. the next mixed slot. Otherwise, if we don't, we
   * might starve HARQ processes that need a retransmission in a specific slot
   * but we might not necessarily reach it */
  bool last_dl = (current.s % fs->numb_slots_period) == (fs->period_cfg.num_dl_slots - 1);
  fsn_t *next = &nr_mac->ul_next;
  while (max_dci > 0) {
    /* go to the next UL slot, skipping DL if necessary */
    *next = fs_get_max(fs, *next, min_next);
    while (!is_ul_slot(next->s, fs))
      *next = fs_add_delta(fs, 1, *next);
    if (!is_dl_slot(current.s, fs)) // if current slot is not DL, nothing to do
      break;

    /* get a TDA so that next UL scheduling slot can be reached from current,
     * and exit if there is no such TDA. Remove koffset, as it is
     * "cell-specific", i.e., the UE will add it on the computation. */
    int k2 = fs_get_diff(fs, *next, current) - koffset;
    DevAssert(k2 > 0);
    // we assume that all beams have the same symbol utilization in all RBs for
    // simplification (but this might not be true). Otherwise, we would need to
    // check it separately for all beams and give a list of TDAs (one for each
    // beam), which is complex.
    int beam = 0;
    const NR_tda_info_t *tda_info = NULL;
    int n_tda = get_num_ul_tda(nr_mac, next->s, k2, &tda_info);
    if (n_tda == 0) /* no TDA fulfills this */
      break;
    int rb_start = 0;
    int rb_len = bw;
    tda_info = get_best_ul_tda(nr_mac, beam, tda_info, n_tda, next->f, next->s, &rb_start, &rb_len);
    DevAssert(tda_info->valid_tda);
    int tda = seq_arr_dist(&nr_mac->ul_tda, seq_arr_front(&nr_mac->ul_tda), tda_info);
    AssertFatal(tda >= 0 && tda < 16, "illegal TDA index %d\n", tda);

    nr_mac->mac_stats.ul.total_prb_aggregate += bw;
    int len[num_beams];
    for (int i = 0; i < num_beams; i++)
      len[i] = rb_len;
    /* UL scheduling via function pointers (beam alloc + policy) */
    int sched = nr_ul_schedule(nr_mac, pp_pusch, tda, tda_info, nr_mac->UE_info.connected_ue_list, max_dci, num_beams, len);
    LOG_D(NR_MAC, "run nr_ul_schedule() at %4d.%2d with tda %d k2 %d (ULSCH at %4d.%2d) scheduled %d last_dl %d\n", frame, slot, tda, k2, next->f, next->s, sched, last_dl);
    /* if we did not schedule anything, and it's not the last slot, break. In
     * the case we did schedule or it's the last slot (see above!), continue
     * advancing till there is no TDA anymore */
    if (sched == 0 && !last_dl)
      break;
    max_dci -= sched;

    *next = fs_add_delta(fs, 1, *next);
  }
}

nr_pp_impl_ul nr_init_ulsch_preprocessor(int CC_id)
{
  return nr_ulsch_preprocessor;
}

void nr_schedule_ulsch(module_id_t module_id, frame_t frame, slot_t slot, nfapi_nr_ul_dci_request_t *ul_dci_req)
{
  gNB_MAC_INST *nr_mac = RC.nrmac[module_id];
  /* already mutex protected: held in gNB_dlsch_ulsch_scheduler() */
  NR_SCHED_ENSURE_LOCKED(&nr_mac->sched_lock);

  ul_dci_req->SFN = frame;
  ul_dci_req->Slot = slot;
  post_process_pusch_t pusch = { .frame = frame, .slot = slot, .ul_dci_req = ul_dci_req, .pdcch_pdu_coreset = {NULL}, };

  nr_mac->pre_processor_ul(nr_mac, &pusch);
}

int collect_ul_candidates(gNB_MAC_INST *mac,
                          NR_UE_info_t *UE_list[],
                          nr_ul_candidate_t *candidates,
                          int max_candidates,
                          frame_t frame, slot_t slot,
                          int sched_frame, int sched_slot,
                          int tda,
                          const NR_tda_info_t *tda_info)
{
  int numUE = 0;

  UE_iterator(UE_list, UE) {
    if (numUE >= max_candidates)
      break;

    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    if (!nr_mac_ue_is_active(UE))
      continue;

    LOG_D(NR_MAC, "collect_ul_candidates: preparing UL scheduling for UE %04x\n", UE->rnti);
    NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;
    NR_mac_dir_stats_t *stats = &UE->mac_stats.ul;

    /* Update goodput EWMA once per frame (10ms), in bps */
    if (frame != stats->last_goodput_frame) {
      double goodput_bps = (double)(stats->total_sdu_bytes - stats->prev_sdu_bytes) * 800.0;
      UE->ul_thr_ue = (1 - 0.1f) * UE->ul_thr_ue + 0.1f * (float)goodput_bps;
      stats->prev_sdu_bytes = stats->total_sdu_bytes;
      stats->last_goodput_frame = frame;
    }
    stats->current_bytes = 0;
    stats->current_rbs = 0;

    bwp_info_t bi = get_pusch_bwp_start_size(UE);
    int nrOfLayers = get_ul_nrOfLayers(sched_ctrl, current_BWP->dci_format);

    nr_ul_candidate_t cand = {0};
    cand.UE = UE;
    cand.beam_index = UE->UE_beam_index;
    cand.alloc_beam_idx = -1;
    cand.bwp_start = bi.bwpStart;
    cand.bwp_size = bi.bwpSize;
    cand.nrOfLayers = nrOfLayers;
    cand.mcs_table = current_BWP->mcs_table;
    cand.avg_throughput = UE->ul_thr_ue;
    cand.ph = sched_ctrl->ph;
    cand.pcmax = sched_ctrl->pcmax;
    cand.rnti = UE->rnti;
    cand.pusch_snrx10 = sched_ctrl->pusch_snrx10;
    cand.target_snrx10 = (UE->ul_snr_target_override >= 0)
        ? UE->ul_snr_target_override
        : mac->pusch_target_snrx10;

    if ((frame & 0x3FF) == 0 && slot == 0)
      LOG_W(NR_MAC, "UE %04x: override=%d global=%d -> target=%d\n",
            UE->rnti, UE->ul_snr_target_override, mac->pusch_target_snrx10, cand.target_snrx10);

    /* fiveQI: extract from first DRB's QoS config */
    cand.fiveQI = 0;
    for (int j = 0; j < seq_arr_size(&sched_ctrl->lc_config); j++) {
      const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, j);
      if (c->lcid >= 4) {
        for (int q = 0; q < NR_MAX_NUM_QFI; q++) {
          if (c->qos_config[q].fiveQI > 0) {
            cand.fiveQI = c->qos_config[q].fiveQI;
            break;
          }
        }
        if (cand.fiveQI > 0) break;
      }
    }

    /* DL wideband CQI from CSI report */
    cand.cqi = sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.wb_cqi_1tb;

    /* DL RSRP averaged over measurements */
    cand.dl_rsrp = UE->mac_stats.num_rsrp_meas > 0
        ? UE->mac_stats.cumul_rsrp / UE->mac_stats.num_rsrp_meas
        : 0;

    /* Check if retransmission is necessary */
    int ul_harq_pid = sched_ctrl->retrans_ul_harq.head;
    LOG_D(NR_MAC, "collect_ul_candidates: UE %04x harq_pid %d\n", UE->rnti, ul_harq_pid);
    if (ul_harq_pid >= 0) {
      const NR_sched_pusch_t *retInfo = &sched_ctrl->ul_harq_processes[ul_harq_pid].sched_pusch;
      cand.is_retx = true;
      cand.retx_harq_pid = ul_harq_pid;
      cand.retx_rbSize = retInfo->rbSize;
      cand.current_mcs = retInfo->mcs;
      cand.bler = sched_ctrl->ul_bler_stats.bler;
      candidates[numUE++] = cand;
      continue;
    }

    /* Skip UE if no free HARQ processes */
    if (sched_ctrl->available_ul_harq.head < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] has no free UL HARQ process, skipping\n", UE->rnti, frame, slot);
      continue;
    }

    const int B = max(0, sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes);
    const bool do_sched = nr_UE_is_to_be_scheduled(&mac->frame_structure,
                                                    UE,
                                                    sched_frame,
                                                    sched_slot,
                                                    mac->ulsch_max_frame_inactivity);

    LOG_D(NR_MAC, "collect_ul_candidates: do_sched UE %04x => %s\n", UE->rnti, do_sched ? "yes" : "no");
    if ((B == 0 && !do_sched) || nr_timer_is_active(&sched_ctrl->transm_interrupt))
      continue;

    /* Calculate MCS from BLER stats */
    const NR_bler_options_t *bo = &mac->ul_bler;
    const int max_mcs_table = (current_BWP->mcs_table == 0 || current_BWP->mcs_table == 2) ? 28 : 27;
    const int max_mcs = min(bo->max_mcs, max_mcs_table);
    int selected_mcs;
    if (bo->harq_round_max == 1) {
      selected_mcs = get_mcs_from_SINRx10(current_BWP->mcs_table, sched_ctrl->pusch_snrx10, nrOfLayers);
      selected_mcs = min(max_mcs, selected_mcs);
      selected_mcs = max(bo->min_mcs, selected_mcs);
      sched_ctrl->ul_bler_stats.mcs = selected_mcs;
    } else {
      selected_mcs = get_mcs_from_bler(bo, stats, &sched_ctrl->ul_bler_stats, max_mcs, frame);
      LOG_D(NR_MAC, "%d.%d starting mcs %d bler %f\n", frame, slot, selected_mcs, sched_ctrl->ul_bler_stats.bler);
    }

    cand.is_retx = false;
    cand.retx_harq_pid = -1;
    cand.sched_inactive = (B == 0 && do_sched);
    cand.pending_bytes = B;
    cand.bler = sched_ctrl->ul_bler_stats.bler;
    cand.current_mcs = selected_mcs;
    cand.max_mcs = max_mcs;

    LOG_D(NR_MAC, "[UE %04x][%4d.%2d] b %d, ul_thr_ue %f, mcs %d, sched_inactive %d\n",
          UE->rnti, frame, slot, B, UE->ul_thr_ue, selected_mcs, cand.sched_inactive);

    candidates[numUE++] = cand;
  }

  {
    nr_ul_candidate_t tmp[MAX_MOBILES_PER_GNB];
    int out = 0;
    /* Pass 1: retransmissions */
    for (int i = 0; i < numUE; i++)
      if (candidates[i].is_retx)
        tmp[out++] = candidates[i];
    /* Pass 2: inactive/SR UEs (newly attached) */
    for (int i = 0; i < numUE; i++)
      if (!candidates[i].is_retx && candidates[i].sched_inactive)
        tmp[out++] = candidates[i];
    /* Pass 3: normal new data */
    for (int i = 0; i < numUE; i++)
      if (!candidates[i].is_retx && !candidates[i].sched_inactive)
        tmp[out++] = candidates[i];
    DevAssert(out == numUE);
    memcpy(candidates, tmp, numUE * sizeof(nr_ul_candidate_t));
  }

  return numUE;
}

int nr_ul_beam_alloc_default(NR_beam_info_t *beam_info,
                             nr_ul_candidate_t *candidates,
                             int n_candidates,
                             frame_t frame, slot_t slot,
                             frame_t sched_frame, slot_t sched_slot,
                             int slots_per_frame)
{
  int out = 0;
  for (int i = 0; i < n_candidates; i++) {
    nr_ul_candidate_t *cand = &candidates[i];

    /* Allocate beam for DCI slot */
    NR_beam_alloc_t dci_beam = beam_allocation_procedure(beam_info, frame, slot,
                                                          cand->beam_index, slots_per_frame);
    if (dci_beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] DCI beam could not be allocated\n",
            cand->UE->rnti, frame, slot);
      continue;
    }

    /* Allocate beam for scheduled PUSCH slot */
    NR_beam_alloc_t sched_beam = beam_allocation_procedure(beam_info, sched_frame, sched_slot,
                                                            cand->beam_index, slots_per_frame);
    if (sched_beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] Sched beam could not be allocated\n",
            cand->UE->rnti, frame, slot);
      reset_beam_status(beam_info, frame, slot, cand->beam_index, slots_per_frame, dci_beam.new_beam);
      continue;
    }

    cand->dci_beam_idx = dci_beam.idx;
    cand->dci_beam_new = dci_beam.new_beam;
    cand->alloc_beam_idx = sched_beam.idx;
    cand->sched_beam_new = sched_beam.new_beam;

    /* Compact: move candidate to output position */
    if (out != i)
      candidates[out] = candidates[i];
    out++;
  }
  return out;
}

static int find_first_free_rbs_ul(const uint16_t *vrb_map, uint16_t slbitmap,
                                  int bwp_start, int bwp_size, int num_rb)
{
  int rbStart = 0;
  while (rbStart < bwp_size && (vrb_map[rbStart + bwp_start] & slbitmap))
    rbStart++;
  /* check contiguous block of num_rb */
  int avail = 0;
  for (int rb = rbStart; rb < bwp_size && avail < num_rb; rb++) {
    if (vrb_map[rb + bwp_start] & slbitmap)
      return -1;
    avail++;
  }
  return avail >= num_rb ? rbStart : -1;
}

static int find_largest_free_block_ul(const uint16_t *vrb_map, uint16_t slbitmap,
                                      int bwp_start, int bwp_size,
                                      int *out_start)
{
  int best_start = 0, best_len = 0;
  int cur_start = 0, cur_len = 0;
  for (int rb = 0; rb < bwp_size; rb++) {
    if (!(vrb_map[rb + bwp_start] & slbitmap)) {
      if (cur_len == 0)
        cur_start = rb;
      cur_len++;
    } else {
      if (cur_len > best_len) {
        best_start = cur_start;
        best_len = cur_len;
      }
      cur_len = 0;
    }
  }
  if (cur_len > best_len) {
    best_start = cur_start;
    best_len = cur_len;
  }
  *out_start = best_start;
  return best_len;
}

bool nr_ul_check_phr(const nr_ul_sched_params_t *params,
                     const nr_ul_candidate_t *cand,
                     uint16_t rbSize, uint8_t mcs,
                     nr_ul_phr_advice_t *advice)
{
  NR_UE_UL_BWP_t *current_BWP = &cand->UE->current_UL_BWP;
  long *deltaMCS = current_BWP->pusch_Config ? current_BWP->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
  NR_pusch_dmrs_t dmrs_info = get_ul_dmrs_params(params->scc, current_BWP, params->tda_info, cand->nrOfLayers);
  int n_dmrs = dmrs_info.N_PRB_DMRS * dmrs_info.num_dmrs_symb;

  uint16_t R;
  uint8_t Qm;
  update_ul_ue_R_Qm(mcs, current_BWP->mcs_table, current_BWP->pusch_Config, &R, &Qm);
  int tbs_bits = nr_compute_tbs(Qm, R, rbSize, params->tda_info->nrOfSymbols, n_dmrs, 0, 0, cand->nrOfLayers);
  int tx_power = compute_ph_factor(current_BWP->scs, tbs_bits, rbSize, cand->nrOfLayers,
                                   params->tda_info->nrOfSymbols, n_dmrs, deltaMCS, true);

  if (cand->ph >= tx_power)
    return true; /* allocation respects PHR */

  /* PHR violated — compute two suggestions */

  /* Suggestion 1: max_mcs_min_rb — reduce RBs first (down to min_rb), then MCS */
  {
    nr_ul_phr_suggestion_t *s = &advice->max_mcs_min_rb;
    uint16_t rb = rbSize;
    uint8_t m = mcs;
    uint16_t Rt = R;
    uint8_t Qt = Qm;
    int tp = tx_power;

    while (cand->ph < tp && rb > params->min_rb) {
      rb--;
      tbs_bits = nr_compute_tbs(Qt, Rt, rb, params->tda_info->nrOfSymbols, n_dmrs, 0, 0, cand->nrOfLayers);
      tp = compute_ph_factor(current_BWP->scs, tbs_bits, rb, cand->nrOfLayers,
                             params->tda_info->nrOfSymbols, n_dmrs, deltaMCS, true);
    }
    while (cand->ph < tp && m > 0) {
      m--;
      update_ul_ue_R_Qm(m, current_BWP->mcs_table, current_BWP->pusch_Config, &Rt, &Qt);
      tbs_bits = nr_compute_tbs(Qt, Rt, rb, params->tda_info->nrOfSymbols, n_dmrs, 0, 0, cand->nrOfLayers);
      tp = compute_ph_factor(current_BWP->scs, tbs_bits, rb, cand->nrOfLayers,
                             params->tda_info->nrOfSymbols, n_dmrs, deltaMCS, true);
    }
    s->rbSize = rb;
    s->mcs = m;
    s->valid = (cand->ph >= tp);
  }

  /* Suggestion 2: same_rb_min_mcs — keep RBs, only reduce MCS */
  {
    nr_ul_phr_suggestion_t *s = &advice->same_rb_min_mcs;
    uint8_t m = mcs;
    uint16_t Rt = R;
    uint8_t Qt = Qm;
    int tp = tx_power;

    while (cand->ph < tp && m > 0) {
      m--;
      update_ul_ue_R_Qm(m, current_BWP->mcs_table, current_BWP->pusch_Config, &Rt, &Qt);
      tbs_bits = nr_compute_tbs(Qt, Rt, rbSize, params->tda_info->nrOfSymbols, n_dmrs, 0, 0, cand->nrOfLayers);
      tp = compute_ph_factor(current_BWP->scs, tbs_bits, rbSize, cand->nrOfLayers,
                             params->tda_info->nrOfSymbols, n_dmrs, deltaMCS, true);
    }
    s->rbSize = rbSize;
    s->mcs = m;
    s->valid = (cand->ph >= tp);
  }

  return false;
}

void nr_ul_proportional_fair(const nr_ul_sched_params_t *params,
                             const nr_ul_candidate_t *candidates,
                             nr_ul_alloc_t *allocs,
                             int n_candidates)
{
  const uint16_t slbitmap = params->slbitmap;
  const int min_rb = params->min_rb;

  /* Work on a local copy of the VRB map so we can track allocations within this call */
  uint16_t vrb_map_local[MAX_BWP_SIZE];
  memcpy(vrb_map_local, params->vrb_map_UL, MAX_BWP_SIZE * sizeof(uint16_t));

  /* Phase 1: Retransmissions — find contiguous free RBs matching retx_rbSize */
  for (int i = 0; i < n_candidates; i++) {
    const nr_ul_candidate_t *cand = &candidates[i];
    if (!cand->is_retx)
      continue;

    int rbStart = find_first_free_rbs_ul(vrb_map_local, slbitmap,
                                         cand->bwp_start, cand->bwp_size, cand->retx_rbSize);
    if (rbStart < 0) {
      /* Try with different TBS match if TDA changed — use nr_find_nb_rb to find
       * the right rbSize for the same TBS. For simplicity in the policy, we just
       * look for the largest free block and let the orchestrator handle TDA mismatch. */
      int block_start;
      int block_len = find_largest_free_block_ul(vrb_map_local, slbitmap,
                                                  cand->bwp_start, cand->bwp_size, &block_start);
      if (block_len >= cand->retx_rbSize) {
        rbStart = block_start;
      } else {
        LOG_D(NR_MAC, "[UE %04x] retx: no %d contiguous free RBs\n",
              cand->UE->rnti, cand->retx_rbSize);
        continue;
      }
    }

    allocs[i].scheduled = true;
    allocs[i].rbStart = rbStart;
    allocs[i].rbSize = cand->retx_rbSize;
    allocs[i].mcs = cand->current_mcs;

    /* Mark RBs used in local map */
    for (int rb = 0; rb < cand->retx_rbSize; rb++)
      vrb_map_local[rbStart + cand->bwp_start + rb] |= slbitmap;
  }

  /* Phase 2: Inactive UEs — allocate min_rb */
  for (int i = 0; i < n_candidates; i++) {
    const nr_ul_candidate_t *cand = &candidates[i];
    if (cand->is_retx || !cand->sched_inactive)
      continue;

    int rbStart = find_first_free_rbs_ul(vrb_map_local, slbitmap,
                                         cand->bwp_start, cand->bwp_size, min_rb);
    if (rbStart < 0)
      continue;

    allocs[i].scheduled = true;
    allocs[i].rbStart = rbStart;
    allocs[i].rbSize = min_rb;
    allocs[i].mcs = cand->current_mcs;

    for (int rb = 0; rb < min_rb; rb++)
      vrb_map_local[rbStart + cand->bwp_start + rb] |= slbitmap;
  }

  /* Phase 3: New data — PF sort, allocate largest free block per UE */

  /* Build PF coefficients for new-data candidates */
  typedef struct {
    float coef;
    int   idx;
  } pf_entry_t;

  pf_entry_t pf_list[MAX_MOBILES_PER_GNB];
  int n_pf = 0;

  for (int i = 0; i < n_candidates; i++) {
    const nr_ul_candidate_t *cand = &candidates[i];
    if (cand->is_retx || cand->sched_inactive)
      continue;

    NR_UE_UL_BWP_t *current_BWP = &cand->UE->current_UL_BWP;
    const uint8_t Qm = nr_get_Qm_ul(cand->current_mcs, current_BWP->mcs_table);
    const uint16_t R = nr_get_code_rate_ul(cand->current_mcs, current_BWP->mcs_table);
    const uint32_t tbs = nr_compute_tbs(Qm, R, 1, 10, 0, 0, 0, cand->nrOfLayers) >> 3;
    float thr = cand->avg_throughput > 0 ? cand->avg_throughput : 1.0f;
    pf_list[n_pf].coef = (float)tbs / thr;
    pf_list[n_pf].idx = i;
    n_pf++;
  }

  /* Sort by PF coefficient descending */
  for (int i = 0; i < n_pf - 1; i++) {
    for (int j = i + 1; j < n_pf; j++) {
      if (pf_list[j].coef > pf_list[i].coef) {
        pf_entry_t tmp = pf_list[i];
        pf_list[i] = pf_list[j];
        pf_list[j] = tmp;
      }
    }
  }

  /* Allocate in PF order */
  for (int p = 0; p < n_pf; p++) {
    int i = pf_list[p].idx;
    const nr_ul_candidate_t *cand = &candidates[i];

    int block_start;
    int block_len = find_largest_free_block_ul(vrb_map_local, slbitmap,
                                                cand->bwp_start, cand->bwp_size, &block_start);
    if (block_len < min_rb)
      continue;

    uint16_t rbSize = block_len;
    uint8_t mcs = cand->current_mcs;

    /* Apply PHR check if UE has reported PHR */
    if (cand->pcmax != 0 || cand->ph != 0) {
      nr_ul_phr_advice_t advice;
      if (!nr_ul_check_phr(params, cand, rbSize, mcs, &advice)) {
        /* Use max_mcs_min_rb suggestion (current PF behavior: shrink RBs first) */
        if (advice.max_mcs_min_rb.valid) {
          rbSize = advice.max_mcs_min_rb.rbSize;
          mcs = advice.max_mcs_min_rb.mcs;
        } else {
          LOG_D(NR_MAC, "[UE %04x] PHR: no feasible allocation\n", cand->UE->rnti);
          continue;
        }
      }
    }

    /* Find optimal RB count for pending data */
    NR_UE_UL_BWP_t *current_BWP = &cand->UE->current_UL_BWP;
    NR_pusch_dmrs_t dmrs_info = get_ul_dmrs_params(params->scc, current_BWP, params->tda_info, cand->nrOfLayers);
    uint16_t Rt;
    uint8_t Qt;
    update_ul_ue_R_Qm(mcs, current_BWP->mcs_table, current_BWP->pusch_Config, &Rt, &Qt);

    uint32_t tb_size;
    uint16_t final_rbSize;
    nr_find_nb_rb(Qt, Rt, current_BWP->transform_precoding,
                  cand->nrOfLayers, params->tda_info->nrOfSymbols,
                  dmrs_info.N_PRB_DMRS * dmrs_info.num_dmrs_symb,
                  cand->pending_bytes, min_rb, rbSize,
                  &tb_size, &final_rbSize);

    allocs[i].scheduled = true;
    allocs[i].rbStart = block_start;
    allocs[i].rbSize = final_rbSize;
    allocs[i].mcs = mcs;

    for (int rb = 0; rb < final_rbSize; rb++)
      vrb_map_local[block_start + cand->bwp_start + rb] |= slbitmap;
  }
}

/*
 * Lua UL scheduler integration
 *
 * The ue_metric_t struct is the FFI-compatible data structure shared between
 * C and the Lua scheduling script. It must match the FFI cdef in the Lua
 * script exactly (field order, types, and packing).
 */

typedef struct {
  uint16_t rnti;
  uint8_t  nr_of_layers;
  uint32_t pending_bytes;
  float    throughput;
  uint8_t  previous_mcs;
  int      ue_type;          /* 0=new_data, 1=retx, 2=inactive */
  uint16_t required_rbs;     /* retx: retx_rbSize, inactive: min_rb, new: 0 */
  uint8_t  required_mcs;     /* retx/inactive: current_mcs, new: 255 */
  int      target_snrx10;
  uint16_t cqi;              /* populated in commit 9 */
  uint16_t rssi;             /* future */
  int      uid;              /* stable UE identifier across RNTI changes */
  int      mcs_table;
  float    bler;
  int      pusch_snrx10;
  int      slot;
  int      frame;
  uint64_t fiveQI;           /* populated in commit 8 */
  int16_t  channel_mag_per_rb[272]; /* future */
  uint32_t bwp_start;
  uint32_t bwp_size;
  int16_t  dl_rsrp;          /* populated in commit 10 */
  uint64_t unused_reserved;
  uint64_t rlc_arrival_bytes; /* future */
  uint32_t rlc_arrival_pkts;  /* future */
  uint32_t rlc_dropped_pkts;  /* future */
  uint32_t rlc_dropped_bytes; /* future */
  uint16_t allocated_rb;      /* output: filled by Lua */
  uint8_t  allocated_mcs;     /* output: filled by Lua */
  uint16_t allocated_rb_start;/* output: filled by Lua */
  char     ul_prb_mask[276];  /* per-UE mask, commit 11 */
} ue_metric_t;

static lua_State *lua_ul_state = NULL;
static pthread_mutex_t lua_ul_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_lua_ul_scheduler(void)
{
  const char *script_path = getenv("LUA_SCHED_UL");
  AssertFatal(script_path != NULL, "LUA_SCHED_UL env var not set\n");

  lua_ul_state = luaL_newstate();
  AssertFatal(lua_ul_state != NULL, "Failed to create Lua state\n");
  luaL_openlibs(lua_ul_state);

  int err = luaL_dofile(lua_ul_state, script_path);
  if (err) {
    LOG_E(NR_MAC, "Lua UL scheduler: failed to load '%s': %s\n",
          script_path, lua_tostring(lua_ul_state, -1));
    lua_close(lua_ul_state);
    lua_ul_state = NULL;
    AssertFatal(false, "Lua UL scheduler initialization failed\n");
  }

  /* Verify that compute_allocations() is defined */
  lua_getglobal(lua_ul_state, "compute_allocations");
  AssertFatal(lua_isfunction(lua_ul_state, -1),
              "Lua script must define compute_allocations() function\n");
  lua_pop(lua_ul_state, 1);

  LOG_I(NR_MAC, "Lua UL scheduler loaded from '%s'\n", script_path);
}

void reload_lua_ul_scheduler(const char *script_path)
{
  pthread_mutex_lock(&lua_ul_mutex);

  if (lua_ul_state) {
    lua_close(lua_ul_state);
    lua_ul_state = NULL;
  }

  lua_ul_state = luaL_newstate();
  if (!lua_ul_state) {
    LOG_E(NR_MAC, "Lua UL reload: failed to create Lua state\n");
    pthread_mutex_unlock(&lua_ul_mutex);
    return;
  }
  luaL_openlibs(lua_ul_state);

  int err = luaL_dofile(lua_ul_state, script_path);
  if (err) {
    LOG_E(NR_MAC, "Lua UL reload: failed to load '%s': %s\n",
          script_path, lua_tostring(lua_ul_state, -1));
    lua_close(lua_ul_state);
    lua_ul_state = NULL;
    pthread_mutex_unlock(&lua_ul_mutex);
    return;
  }

  lua_getglobal(lua_ul_state, "compute_allocations");
  if (!lua_isfunction(lua_ul_state, -1)) {
    LOG_E(NR_MAC, "Lua UL reload: compute_allocations() not found in '%s'\n", script_path);
    lua_close(lua_ul_state);
    lua_ul_state = NULL;
    pthread_mutex_unlock(&lua_ul_mutex);
    return;
  }
  lua_pop(lua_ul_state, 1);

  LOG_I(NR_MAC, "Lua UL scheduler reloaded from '%s'\n", script_path);
  pthread_mutex_unlock(&lua_ul_mutex);
}

void set_lua_ul_scheduler_config(int fwa_max_throughput, int mtc_max_throughput)
{
  pthread_mutex_lock(&lua_ul_mutex);
  if (!lua_ul_state) {
    LOG_E(NR_MAC, "Lua UL scheduler not initialized, cannot set config\n");
    pthread_mutex_unlock(&lua_ul_mutex);
    return;
  }

  lua_pushinteger(lua_ul_state, fwa_max_throughput);
  lua_setglobal(lua_ul_state, "fwa_max_throughput");

  lua_pushinteger(lua_ul_state, mtc_max_throughput);
  lua_setglobal(lua_ul_state, "mtc_max_throughput");

  pthread_mutex_unlock(&lua_ul_mutex);
  LOG_I(NR_MAC, "Lua UL scheduler config: fwa_max_throughput=%d Mbps, mtc_max_throughput=%d Mbps\n",
        fwa_max_throughput, mtc_max_throughput);
}

void nr_ul_lua_policy(const nr_ul_sched_params_t *params,
                      const nr_ul_candidate_t *candidates,
                      nr_ul_alloc_t *allocs,
                      int n_candidates)
{
  if (!lua_ul_state || n_candidates == 0)
    return;

  /* Convert candidates to ue_metric_t array */
  ue_metric_t metrics[MAX_MOBILES_PER_GNB];
  memset(metrics, 0, sizeof(metrics));

  for (int i = 0; i < n_candidates; i++) {
    const nr_ul_candidate_t *cand = &candidates[i];
    ue_metric_t *m = &metrics[i];

    m->rnti = cand->rnti;
    m->nr_of_layers = cand->nrOfLayers;
    m->pending_bytes = cand->pending_bytes;
    m->throughput = cand->avg_throughput;
    m->previous_mcs = cand->current_mcs;
    m->mcs_table = cand->mcs_table;
    m->target_snrx10 = cand->target_snrx10;
    m->pusch_snrx10 = cand->pusch_snrx10;
    m->bler = cand->bler;
    m->frame = params->frame;
    m->slot = params->slot;
    m->bwp_start = cand->bwp_start;
    m->bwp_size = cand->bwp_size;
    m->fiveQI = cand->fiveQI;
    m->cqi = cand->cqi;
    m->dl_rsrp = cand->dl_rsrp;
    m->uid = cand->UE->uid;

    if (cand->is_retx)
      m->ue_type = 1;
    else if (cand->sched_inactive)
      m->ue_type = 2;
    else
      m->ue_type = 0;

    if (cand->is_retx) {
      m->required_rbs = cand->retx_rbSize;
      m->required_mcs = cand->current_mcs;
    } else if (cand->sched_inactive) {
      m->required_rbs = params->min_rb;
      m->required_mcs = cand->current_mcs;
    } else {
      m->required_rbs = 0;
      m->required_mcs = 255;
    }

    /* Per-UE PRB mask from UE struct (initialized to all '.' on UE creation) */
    if (cand->UE->ul_blocked_prb_mask[0] != '\0')
      memcpy(m->ul_prb_mask, cand->UE->ul_blocked_prb_mask, 276);
    else {
      memset(m->ul_prb_mask, '.', 275);
      m->ul_prb_mask[275] = '\0';
    }
  }

  /* Build RB mask string from VRB map, then apply global blocked mask */
  char rb_mask[MAX_BWP_SIZE + 1];
  const uint16_t slbitmap = params->slbitmap;
  for (int rb = 0; rb < MAX_BWP_SIZE; rb++) {
    if ((params->vrb_map_UL[rb] & slbitmap) || g_blocked_prb_mask[rb] == 'X')
      rb_mask[rb] = 'X';
    else
      rb_mask[rb] = '.';
  }
  rb_mask[MAX_BWP_SIZE] = '\0';

  /* Call Lua: compute_allocations(metrics_ptr, n_ues, total_rbs, min_rbs, rb_mask_string) */
  pthread_mutex_lock(&lua_ul_mutex);
  if (!lua_ul_state) {
    pthread_mutex_unlock(&lua_ul_mutex);
    return;
  }

  lua_getglobal(lua_ul_state, "compute_allocations");
  lua_pushlightuserdata(lua_ul_state, metrics);
  lua_pushinteger(lua_ul_state, n_candidates);
  lua_pushinteger(lua_ul_state, params->n_rb_avail);
  lua_pushinteger(lua_ul_state, params->min_rb);
  lua_pushstring(lua_ul_state, rb_mask);

  int err = lua_pcall(lua_ul_state, 5, 0, 0);
  if (err) {
    LOG_E(NR_MAC, "Lua UL scheduler error: %s\n", lua_tostring(lua_ul_state, -1));
    lua_pop(lua_ul_state, 1);
    pthread_mutex_unlock(&lua_ul_mutex);
    return;
  }
  pthread_mutex_unlock(&lua_ul_mutex);

  /* Read back allocations from metrics struct (Lua writes directly into it) */
  for (int i = 0; i < n_candidates; i++) {
    if (metrics[i].allocated_rb > 0) {
      allocs[i].scheduled = true;
      allocs[i].rbStart = metrics[i].allocated_rb_start;
      allocs[i].rbSize = metrics[i].allocated_rb;
      allocs[i].mcs = metrics[i].allocated_mcs;
    }
  }
}
