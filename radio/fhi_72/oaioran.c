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
#include <string.h>
#include <stdlib.h>
#include "xran_fh_o_du.h"
#include "xran_compression.h"
#include "armral_bfp_compression.h"

#if defined(__arm__) || defined(__aarch64__)
#else
// xran_cp_api.h uses SIMD, but does not include it
#include <immintrin.h>
#endif
#include "xran_cp_api.h"
#include "xran_sync_api.h"
#include "oran_isolate.h"
#include "oran-init.h"
#include "oaioran.h"
#include <rte_ethdev.h>

#include "oran-config.h" // for g_kbar

#include "common/utils/threadPool/notified_fifo.h"

#define N_SC_PER_PRB 12

#if OAI_FHI72_USE_POLLING
#define USE_POLLING
#endif

// Declare variable useful for the send buffer function
volatile bool first_call_set = false;

int xran_is_prach_slot(uint8_t PortId, uint32_t subframe_id, uint32_t slot_id);
#include "common/utils/LOG/log.h"

#ifndef USE_POLLING
extern notifiedFIFO_t oran_sync_fifo;
#else
volatile oran_sync_info_t oran_sync_info = {0};
#endif

/** @details xran-specific callback, called when all packets for given CC and
 * 1/4, 1/2, 3/4, all symbols of a slot arrived. Currently, only used to get
 * timing information and unblock another thread in xran_fh_rx_read_slot()
 * through either a message queue, or writing in global memory with polling, on
 * a full slot boundary. */
void oai_xran_fh_rx_callback(void *pCallbackTag, xran_status_t status)
{
  struct xran_cb_tag *callback_tag = (struct xran_cb_tag *)pCallbackTag;

  static int32_t last_slot = -1;
  static int32_t last_frame = -1;

  const struct xran_fh_init *fh_init = get_xran_fh_init();
  int num_ports = fh_init->xran_ports;

  /* assuming all RUs have the same numerology */
  const struct xran_fh_config *fh_cfg = get_xran_fh_config(0);
  const int slots_in_sf = 1 << fh_cfg->frame_conf.nNumerology;
  const int sf_in_frame = 10;

  static int rx_RU[XRAN_PORTS_NUM][160] = {0};
  uint32_t tti = callback_tag->slotiId;
  uint32_t frame = XranGetFrameNum(tti, 0, sf_in_frame, slots_in_sf);
  uint32_t subframe = XranGetSubFrameNum(tti, slots_in_sf, sf_in_frame);
  uint32_t slot = XranGetSlotNum(tti, slots_in_sf);

  uint32_t rx_sym = callback_tag->symbol & 0xFF;
  uint32_t ru_id = callback_tag->oXuId;

  LOG_D(HW, "rx_callback at %4d.%3d (subframe %d), rx_sym %d ru_id %d\n", frame, slot, subframe, rx_sym, ru_id);

  if (rx_sym == 7) { // in F release this value is defined as XRAN_FULL_CB_SYM (full slot (offset + 7))
#ifdef F_RELEASE
    for (int ru_idx = 0; ru_idx < num_ports; ru_idx++) {
      struct xran_fh_config *fh_config = get_xran_fh_config(ru_idx);
      oran_buf_list_t *bufs = get_xran_buffers(ru_idx);
      for (uint16_t cc_id = 0; cc_id < 1 /* fh_config->nCC */; cc_id++) { // OAI does not support multiple CC yet.
        for(uint32_t ant_id = 0; ant_id < fh_config->neAxc; ant_id++) {
          struct xran_prb_map *pRbMap = (struct xran_prb_map *)bufs->dstcp[ant_id][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
          AssertFatal(pRbMap != NULL, "(%d:%d:%d)pRbMap == NULL. Aborting.\n", cc_id, tti % XRAN_N_FE_BUF_LEN, ant_id);

          for (uint32_t sym_id = 0; sym_id < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_id++) {
            LOG_D(HW, "cb pRbMap->nPrbElm %d\n", pRbMap->nPrbElm);
            for (uint32_t idxElm = 0; idxElm < pRbMap->nPrbElm; idxElm++ ) {
              struct xran_prb_elm *pRbElm = &pRbMap->prbMap[idxElm];
              pRbElm->nSecDesc[sym_id] = 0; // number of section descriptors per symbol; M-plane info <supported-section-types>
            }
          }
        }
      }
    }
#endif
    // if xran did not call xran_physide_dl_tti callback, it's not ready yet.
    // wait till first callback to advance counters, because otherwise users
    // would see periodic output with only "0" in stats counters
    if (!first_call_set)
      return;
    uint32_t slot2 = slot + (subframe * slots_in_sf);
    rx_RU[ru_id][slot2] = 1;
    if (last_frame > 0 && frame > 0
        && ((slot2 > 0 && last_frame != frame) || (slot2 == 0 && last_frame != ((1024 + frame - 1) & 1023))))
      LOG_E(HW, "Jump in frame counter last_frame %d => %d, slot %d\n", last_frame, frame, slot2);
    for (int i = 0; i < num_ports; i++) {
      if (rx_RU[i][slot2] == 0)
        return;
    }
    for (int i = 0; i < num_ports; i++)
      rx_RU[i][slot2] = 0;

    if (last_slot == -1 || slot2 != last_slot) {
#ifndef USE_POLLING
      notifiedFIFO_elt_t *req = newNotifiedFIFO_elt(sizeof(oran_sync_info_t), 0, &oran_sync_fifo, NULL);
      oran_sync_info_t *info = NotifiedFifoData(req);
      info->tti = tti;
      info->sl = slot2;
      info->f = frame;
      LOG_D(HW, "Push %d.%d.%d (slot %d, subframe %d,last_slot %d)\n", frame, info->sl, slot, ru_id, subframe, last_slot);
      pushNotifiedFIFO(&oran_sync_fifo, req);
#else
      LOG_D(HW, "Writing %d.%d.%d (slot %d, subframe %d,last_slot %d)\n", frame, slot2, ru_id, slot, subframe, last_slot);
      oran_sync_info.tti = tti;
      oran_sync_info.sl = slot2;
      oran_sync_info.f = frame;
#endif
    } else
      LOG_E(HW, "Cannot Push %d.%d.%d (slot %d, subframe %d,last_slot %d)\n", frame, slot2, ru_id, slot, subframe, last_slot);
    last_slot = slot2;
    last_frame = frame;
  } // rx_sym == 7
}

/** @details Only used to unblock timing in oai_xran_fh_rx_callback() on first
 * call. */
int oai_physide_dl_tti_call_back(void *param)
{
  if (!first_call_set)
    LOG_I(HW, "first_call set from phy cb\n");
  first_call_set = true;
  return 0;
}

/** @brief Reads PRACH data from xran buffers.
 *
 * @details Reads PRACH data from xran-specific buffers and, if I/Q compression
 * (bitwidth < 16 bits) is configured, uncompresses the data. Places PRACH data
 * in OAI buffer. */
static int read_prach_data(ru_info_t *ru, int frame, int slot)
{
  /* calculate tti and subframe_id from frame, slot num */
  int sym_idx = 0;

  struct xran_fh_init *fh_init = get_xran_fh_init();
  struct xran_fh_config *fh_cfg = get_xran_fh_config(0);
  nr_prach_info_t prach_info = get_prach_info(0);

  int prach_start_sym = prach_info.start_symbol;
  int prach_end_sym = prach_info.N_dur + prach_start_sym;
  struct xran_ru_config *ru_conf = &fh_cfg->ru_conf;
  int slots_per_frame = 10 << fh_cfg->frame_conf.nNumerology;
  int slots_per_subframe = 1 << fh_cfg->frame_conf.nNumerology;

  int tti = slots_per_frame * (frame) + (slot);
  uint32_t subframe = slot / slots_per_subframe;
  // PRACH occasion in a frame if and only if SFN % x == y, TS 38.211 Table 6.3.3.2-2/3/4
  uint32_t is_prach_frame = (frame % prach_info.x == prach_info.y);
  uint32_t is_prach_slot = is_prach_frame && xran_is_prach_slot(0, subframe, (slot % slots_per_subframe));

  int nb_rx_per_ru = ru->nb_rx / fh_init->xran_ports;
  /* If it is PRACH slot, copy prach IQ from XRAN PRACH buffer to OAI PRACH buffer */
  if (is_prach_slot) {
    if (!ru->prach_buf) {
      LOG_W(HW, "we get rach data from ru, but it is not scheduled %d.%d\n", frame, slot);
      return -1;
    }
    for (sym_idx = prach_start_sym; sym_idx < prach_end_sym; sym_idx++) {
      for (int aa = 0; aa < ru->nb_rx; aa++) {
        int16_t *dst, *src;
        int idx = 0;
        oran_buf_list_t *bufs = get_xran_buffers(aa / nb_rx_per_ru);
        // hardcoded to use only first prach occasion
        dst = (int16_t *)ru->prach_buf[0][aa];
        src = (int16_t *)bufs->prachdstdecomp[aa % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers[sym_idx].pData;
        /* convert Network order to host order */
        if (ru_conf->compMeth_PRACH == XRAN_COMPMETHOD_NONE) {
          if (sym_idx == prach_start_sym) {
            for (idx = 0; idx < 139 * 2; idx++) {
              dst[idx] = ((int16_t)ntohs(src[idx + g_kbar]));
            }
          } else {
            for (idx = 0; idx < 139 * 2; idx++) {
              dst[idx] += ((int16_t)ntohs(src[idx + g_kbar]));
            }
          }
        } else if (ru_conf->compMeth_PRACH == XRAN_COMPMETHOD_BLKFLOAT) {

          int16_t local_dst[12 * 2 * N_SC_PER_PRB] __attribute__((aligned(64)));

#if defined(__i386__) || defined(__x86_64__)
          struct xranlib_decompress_request bfp_decom_req = {};
          struct xranlib_decompress_response bfp_decom_rsp = {};
          int payload_len = (3 * ru_conf->iqWidth_PRACH + 1) * 12; // 12 = closest number of PRBs to 139 REs

          bfp_decom_req.data_in = (int8_t *)src;
          bfp_decom_req.numRBs = 12; // closest number of PRBs to 139 REs
          bfp_decom_req.len = payload_len;
          bfp_decom_req.compMethod = XRAN_COMPMETHOD_BLKFLOAT;
          bfp_decom_req.iqWidth = ru_conf->iqWidth_PRACH;

          bfp_decom_rsp.data_out = (int16_t *)local_dst;
          bfp_decom_rsp.len = 0;
          xranlib_decompress_avx512(&bfp_decom_req, &bfp_decom_rsp);
#elif defined(__arm__) || defined(__aarch64__)
          armral_bfp_decompression(ru_conf->iqWidth_PRACH, 12, (int8_t *)src, (int16_t *)local_dst);
#else
          AssertFatal(1 == 0, "BFP decompression not supported on this architecture");
#endif
          // note: this is hardwired for 139 point PRACH sequence, kbar=2
          if (sym_idx == prach_start_sym)
            for (idx = 0; idx < (139 * 2); idx++)
              dst[idx] = local_dst[idx + g_kbar];
          else
            for (idx = 0; idx < (139 * 2); idx++)
              dst[idx] += (local_dst[idx + g_kbar]);
        } // COMPMETHOD_BLKFLOAT
      } // aa
    } // symb_indx
  } // is_prach_slot
  return (0);
}

/** @brief Check if symbol in slot is UL.
 *
 * @param frame_conf xran frame configuration
 * @param slot the current (absolute) slot (number)
 * @param sym_idx the current symbol index */
static bool is_tdd_ul_symbol(const struct xran_frame_config *frame_conf, int slot, int sym_idx)
{
  /* in FDD, every symbol is also UL */
  if (frame_conf->nFrameDuplexType == XRAN_FDD)
    return true;
  int tdd_period = frame_conf->nTddPeriod;
  int slot_in_period = slot % tdd_period;
  /* check if symbol is UL */
  return frame_conf->sSlotConfig[slot_in_period].nSymbolType[sym_idx] == 1 /* UL */;
}

/** @brief Check if symbol in slot is DL.
 *
 * @param frame_conf xran frame configuration
 * @param slot the current (absolute) slot (number)
 * @param sym_idx the current symbol index */
static bool is_tdd_dl_symbol(const struct xran_frame_config *frame_conf, int slot, int sym_idx)
{
  /* in FDD, every symbol is also UL */
  if (frame_conf->nFrameDuplexType == XRAN_FDD)
    return true;
  int tdd_period = frame_conf->nTddPeriod;
  int slot_in_period = slot % tdd_period;
  /* check if symbol is UL */
  return frame_conf->sSlotConfig[slot_in_period].nSymbolType[sym_idx] == 0 /* DL */;
}

/** @brief Check if current slot is guard/mixed */
static bool is_tdd_guard_slot(const struct xran_frame_config *frame_conf, int slot)
{
  return (is_tdd_dl_symbol(frame_conf, slot, 0) && is_tdd_ul_symbol(frame_conf, slot,  XRAN_NUM_OF_SYMBOL_PER_SLOT - 1));
}

/** @brief Check if current slot is DL or guard/mixed without UL (i.e., current
 * slot is not UL). */
static bool is_tdd_dl_guard_slot(const struct xran_frame_config *frame_conf, int slot)
{
  return !is_tdd_ul_symbol(frame_conf, slot, 0);
}

/** @brief Check if current slot is UL or guard/mixed without UL (i.e., current
 * slot is not UL). */
static bool is_tdd_ul_guard_slot(const struct xran_frame_config *frame_conf, int slot)
{
  return is_tdd_ul_symbol(frame_conf, slot, XRAN_NUM_OF_SYMBOL_PER_SLOT - 1);
}

/** @details Read PRACH and PUSCH data from xran buffers.  If
 * I/Q compression (bitwidth < 16 bits) is configured, deccompresses the data
 * before writing. Prints ON TIME counters every 128 frames.
 *
 * Function is blocking and waits for next frame/slot combination. It is unblocked
 * by oai_xran_fh_rx_callback(). It writes the current slot into parameters
 * frame/slot. */
int xran_fh_rx_read_slot(ru_info_t *ru, int *frame, int *slot)
{
  void *ptr = NULL;
  int32_t *pos = NULL;
  int idx = 0;

  static int64_t old_rx_counter[XRAN_PORTS_NUM] = {0};
  static int64_t old_tx_counter[XRAN_PORTS_NUM] = {0};
  struct xran_common_counters x_counters[XRAN_PORTS_NUM];
  static int outcnt = 0;
#ifndef USE_POLLING
  // pull next even from oran_sync_fifo
  notifiedFIFO_elt_t *res = pullNotifiedFIFO(&oran_sync_fifo);

  notifiedFIFO_elt_t *f;
  while ((f = pollNotifiedFIFO(&oran_sync_fifo)) != NULL) {
    oran_sync_info_t *old_info = NotifiedFifoData(res);
    oran_sync_info_t *new_info = NotifiedFifoData(f);
    LOG_E(HW, "Detected double sync message %d.%d => %d.%d\n", old_info->f, old_info->sl, new_info->f, new_info->sl);
    delNotifiedFIFO_elt(res);
    res = f;
  }

  oran_sync_info_t *info = NotifiedFifoData(res);

  *slot = info->sl;
  *frame = info->f;
  delNotifiedFIFO_elt(res);
#else
  *slot = oran_sync_info.sl;
  *frame = oran_sync_info.f;
  uint32_t tti_in = oran_sync_info.tti;

  static int last_slot = -1;
  LOG_D(HW, "oran slot %d, last_slot %d\n", *slot, last_slot);
  int cnt = 0;
  // while (*slot == last_slot)  {
  while (tti_in == oran_sync_info.tti) {
    //*slot = oran_sync_info.sl;
    cnt++;
  }
  LOG_D(HW, "cnt %d, Reading %d.%d\n", cnt, *frame, *slot);
  last_slot = *slot;
#endif
  // return(0);

  struct xran_fh_config *fh_cfg = get_xran_fh_config(0);
  int slots_per_frame = 10 << fh_cfg->frame_conf.nNumerology;

  int tti = slots_per_frame * (*frame) + (*slot);

  read_prach_data(ru, *frame, *slot);

  const struct xran_fh_init *fh_init = get_xran_fh_init();
  int fftsize = 1 << fh_cfg->nULFftSize;

  int slot_offset_rxdata = 3 & (*slot);
  uint32_t slot_size = 4 * 14 * fftsize;
  uint8_t *rx_data = (uint8_t *)ru->rxdataF[0];
  uint8_t *start_ptr = NULL;
  int nb_rx_per_ru = ru->nb_rx / fh_init->xran_ports;
  for (uint16_t cc_id = 0; cc_id < 1 /*nSectorNum*/; cc_id++) { // OAI does not support multiple CC yet.
    for (uint8_t ant_id = 0; ant_id < ru->nb_rx; ant_id++) {
      rx_data = (uint8_t *)ru->rxdataF[ant_id];
      start_ptr = rx_data + (slot_size * slot_offset_rxdata);
      const struct xran_frame_config *frame_conf = &get_xran_fh_config(ant_id / nb_rx_per_ru)->frame_conf;
      // skip processing this slot is TX (no RX in this slot)
      if (!is_tdd_ul_guard_slot(frame_conf, *slot))
        continue;
      // This loop would better be more inner to avoid confusion and maybe also errors.
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        /* the callback is for mixed and UL slots. In mixed, we have to
         * skip DL and guard symbols. */
        if (!is_tdd_ul_symbol(frame_conf, *slot, sym_idx))
          continue;

        oran_buf_list_t *bufs = get_xran_buffers(ant_id / nb_rx_per_ru);
        uint8_t *pPrbMapData = bufs->dstcp[ant_id % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
        struct xran_prb_map *pRbMap = (struct xran_prb_map *)pPrbMapData;

        uint8_t *src = (uint8_t *)ptr;

        // even when the fragmentation occurs, nRBSize & nRBStart carry the same values in each prbMap
        // therefore, I took the liberty to just extract these values from the first prbMap
        int num_totalRB = pRbMap->prbMap[0].nRBSize;
        int start_totalRB = pRbMap->prbMap[0].nRBStart;
        int32_t local_dst[num_totalRB * N_SC_PER_PRB] __attribute__((aligned(64)));

        LOG_D(HW, "[%d.%d] pRbMap->nPrbElm %d\n", *frame, *slot, pRbMap->nPrbElm);
        for (uint32_t idxElm = 0; idxElm < pRbMap->nPrbElm; idxElm++) {
          int numRB, startRB;
          uint8_t *pData;
          struct xran_section_desc *p_sec_desc = NULL;
          struct xran_prb_elm *pRbElm = &pRbMap->prbMap[idxElm];
#ifdef E_RELEASE
          uint32_t one_rb_size =
              (((pRbElm->iqWidth == 0) || (pRbElm->iqWidth == 16)) ? (N_SC_PER_PRB * 2 * 2) : (3 * pRbElm->iqWidth + 1));
          if (fh_init->mtu < num_totalRB * one_rb_size)
            pData = bufs->dst[ant_id % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN]
                        .pBuffers[sym_idx % XRAN_NUM_OF_SYMBOL_PER_SLOT]
                        .pData;
          else {
            p_sec_desc = pRbElm->p_sec_desc[sym_idx][0];
            pData = p_sec_desc->pData;
          }
          numRB = num_totalRB;
          startRB = start_totalRB;
          {
            {
#elif defined F_RELEASE
          // UP_nRBSize & UP_nRBStart are for DL U-plane only
          LOG_D(HW, "[%d.%d] idxElm[%d] startSym[%d]:numSym[%d] UP_startRB[%d]:UP_numRB[%d] sym_idx[%d] ant_id[%d] pRbElm->nRBStart[%d]:pRbElm->nRBSize[%d]\n", *frame, *slot, idxElm, pRbElm->nStartSymb, pRbElm->numSymb, pRbElm->UP_nRBStart, pRbElm->UP_nRBSize, sym_idx, ant_id, pRbElm->nRBStart, pRbElm->nRBSize);
          for (int idxDesc = 0; idxDesc < XRAN_MAX_FRAGMENT; idxDesc++) {
            p_sec_desc = &pRbElm->sec_desc[sym_idx][idxDesc];
            if (p_sec_desc == NULL)
              continue;
            if (sym_idx >= pRbElm->nStartSymb && sym_idx < pRbElm->nStartSymb + pRbElm->numSymb) {
              if (!p_sec_desc->pCtrl)
                continue;
              pData = p_sec_desc->pData;
              numRB = p_sec_desc->num_prbu;
              startRB = p_sec_desc->start_prbu;
              // num_prbu & start_prbu are for UL U-plane only
              LOG_D(HW, "p_sec_desc[%d] startRB[%d]:numRB[%d]\n", idxDesc, startRB, numRB);
#endif
              ptr = pData;
              pos = (int32_t *)(start_ptr + (4 * sym_idx * fftsize));
              if (ptr == NULL || pos == NULL)
                continue;
              src = pData;
              if (pRbElm->compMethod == XRAN_COMPMETHOD_NONE) {
                // NOTE: gcc 11 knows how to generate AVX2 for this!
                for (idx = 0; idx < (numRB * N_SC_PER_PRB) * 2; idx++)
                  ((int16_t *)local_dst)[idx + startRB * N_SC_PER_PRB * 2] = ((int16_t)ntohs(((uint16_t *)src)[idx])) >> 2;
              } else if (pRbElm->compMethod == XRAN_COMPMETHOD_BLKFLOAT) {
#if defined(__i386__) || defined(__x86_64__)
                struct xranlib_decompress_request bfp_decom_req = {};
                struct xranlib_decompress_response bfp_decom_rsp = {};

                int16_t payload_len = (3 * pRbElm->iqWidth + 1) * numRB;

                bfp_decom_req.data_in = (int8_t *)src;
                bfp_decom_req.numRBs = numRB;
                bfp_decom_req.len = payload_len;
                bfp_decom_req.compMethod = pRbElm->compMethod;
                bfp_decom_req.iqWidth = pRbElm->iqWidth;

                bfp_decom_rsp.data_out = (int16_t *) (local_dst + startRB * N_SC_PER_PRB);
                bfp_decom_rsp.len = 0;

                xranlib_decompress_avx512(&bfp_decom_req, &bfp_decom_rsp);
#elif defined(__arm__) || defined(__aarch64__)
                armral_bfp_decompression(pRbElm->iqWidth, numRB, (int8_t *)src, (int16_t *)local_dst);
#else
                AssertFatal(1 == 0, "BFP compression not supported on this architecture");
#endif
                outcnt++;
              } else {
                printf("pRbElm->compMethod == %d is not supported\n", pRbElm->compMethod);
                exit(-1);
              }
              if ((startRB + numRB) == (start_totalRB + num_totalRB)) {
                int pos_len = 0;
                int neg_len = 0;

                if (start_totalRB < (num_totalRB >> 1)) // there are PRBs left of DC
                  neg_len = min((num_totalRB * 6) - (start_totalRB * 12), num_totalRB * N_SC_PER_PRB);
                pos_len = (num_totalRB * N_SC_PER_PRB) - neg_len;
                // Calculation of the pointer for the section in the buffer.
                // positive half
                uint8_t *dst1 = (uint8_t *)(pos + (neg_len == 0 ? ((start_totalRB * N_SC_PER_PRB) - (num_totalRB * 6)) : 0));
                // negative half
                uint8_t *dst2 = (uint8_t *)(pos + (start_totalRB * N_SC_PER_PRB) + fftsize - (num_totalRB * 6));
                memcpy((void *)dst2, (void *)local_dst, neg_len * 4);
                memcpy((void *)dst1, (void *)&local_dst[neg_len], pos_len * 4);
              }
            }
          } // idxDesc
        } // idxElm

      } // sym_ind
    } // ant_ind
  } // vv_inf
  if ((*frame & 0x7f) == 0 && *slot == 0 && xran_get_common_counters(gxran_handle, &x_counters[0]) == XRAN_STATUS_SUCCESS) {
    for (int o_xu_id = 0; o_xu_id < fh_init->xran_ports; o_xu_id++) {
      LOG_I(HW,
            "[%s%d][rx %7ld pps %7ld kbps %7ld][tx %7ld pps %7ld kbps %7ld][Total Msgs_Rcvd %ld]\n",
            "o-du ",
            o_xu_id,
            x_counters[o_xu_id].rx_counter,
            x_counters[o_xu_id].rx_counter - old_rx_counter[o_xu_id],
            x_counters[o_xu_id].rx_bytes_per_sec * 8 / 1000L,
            x_counters[o_xu_id].tx_counter,
            x_counters[o_xu_id].tx_counter - old_tx_counter[o_xu_id],
            x_counters[o_xu_id].tx_bytes_per_sec * 8 / 1000L,
            x_counters[o_xu_id].Total_msgs_rcvd);
      for (int rxant = 0; rxant < ru->nb_rx / fh_init->xran_ports; rxant++)
        LOG_I(HW,
              "[%s%d][pusch%d %7ld prach%d %7ld]\n",
              "o_du",
              o_xu_id,
              rxant,
              x_counters[o_xu_id].rx_pusch_packets[rxant],
              rxant,
              x_counters[o_xu_id].rx_prach_packets[rxant]);
      if (x_counters[o_xu_id].rx_counter > old_rx_counter[o_xu_id])
        old_rx_counter[o_xu_id] = x_counters[o_xu_id].rx_counter;
      if (x_counters[o_xu_id].tx_counter > old_tx_counter[o_xu_id])
        old_tx_counter[o_xu_id] = x_counters[o_xu_id].tx_counter;
    }
  }
  return (0);
}

/** @details Write PDSCH IQ-data from OAI txdataF_BF buffer to xran buffers. If
 * I/Q compression (bitwidth < 16 bits) is configured, compresses the data
 * before writing. */
int xran_fh_tx_send_slot(ru_info_t *ru, int frame, int slot, uint64_t timestamp)
{
  int tti = /*frame*SUBFRAMES_PER_SYSTEMFRAME*SLOTNUM_PER_SUBFRAME+*/ 20 * frame
            + slot; // commented out temporarily to check that compilation of oran 5g is working.

  void *ptr = NULL;
  int32_t *pos = NULL;
  int idx = 0;

  const struct xran_fh_init *fh_init = get_xran_fh_init();
  const struct xran_fh_config *fh_cfg = get_xran_fh_config(0);
  int fftsize = 1 << fh_cfg->nDLFftSize;
  int nb_tx_per_ru = ru->nb_tx / fh_init->xran_ports;
  int nb_rx_per_ru = ru->nb_rx / fh_init->xran_ports;

  // Handle CP UL packet here instead of at xran_fh_rx_read_slot() as oran_fh_if4p5_south_in() lags behind
  // oran_fh_if4p5_south_out() (which is invoked at the right time slot) by 4 slots.
  // Need to use --continuous-tx so that this routine will be triggered in RX slot.
  for (uint16_t cc_id = 0; cc_id < 1 /*nSectorNum*/; cc_id++) { // OAI does not support multiple CC yet.
    for (uint8_t ant_id = 0; ant_id < ru->nb_rx; ant_id++) {
      int first = 1; // The first UL symbol
      const struct xran_frame_config *frame_conf = &get_xran_fh_config(ant_id / nb_rx_per_ru)->frame_conf;
      // skip processing this slot is TX (no RX in this slot)
      if (!is_tdd_ul_guard_slot(frame_conf, slot)) {
        continue;
      }
      // This loop would better be more inner to avoid confusion and maybe also errors.
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        /* skip DL and guard symbols. */
        if (!is_tdd_ul_symbol(frame_conf, slot, sym_idx)) {
          continue;
        }
        oran_buf_list_t *bufs = get_xran_buffers(ant_id / nb_rx_per_ru);
        uint8_t *pPrbMapData = bufs->dstcp[ant_id % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
        struct xran_prb_map *pPrbMap = (struct xran_prb_map *)pPrbMapData;

        LOG_D(HW, "pPrbMap->nPrbElm %d\n", pPrbMap->nPrbElm);
        for (uint32_t idxElm = 0; idxElm < pPrbMap->nPrbElm; idxElm++) {
          struct xran_prb_elm *pRbElm = &pPrbMap->prbMap[idxElm];
          int numRB, startRB;
#ifdef E_RELEASE
          numRB = pRbElm->nRBSize;
          startRB = pRbElm->nRBStart;
#elif F_RELEASE
          numRB = pRbElm->UP_nRBSize;
          startRB = pRbElm->UP_nRBStart;
#endif
          LOG_D(HW, "pPrbMap[%d] : PRBstart %d nPRBs %d\n", idxElm, startRB, numRB);
          if (first) {
            // ant_id / no of antenna per beam gives the beam_nb
            pRbElm->nBeamIndex =
                ru->beam_id[ant_id / (ru->nb_rx / ru->num_beams_period)][slot * XRAN_NUM_OF_SYMBOL_PER_SLOT + sym_idx];
            // In phy-f-1.0/fhi_lib/lib/api/xran_pkt_cp.h, beamId:15 is of 15bit. -1 set extension bit ef:1 to 1 mistakenly.
            if (pRbElm->nBeamIndex == -1) {
              pRbElm->nBeamIndex = 0;
            } else {
              first = 0;
            }
          }
        }
      }
    }
  }

  for (uint16_t cc_id = 0; cc_id < 1 /*nSectorNum*/; cc_id++) { // OAI does not support multiple CC yet.
    for (uint8_t ant_id = 0; ant_id < ru->nb_tx; ant_id++) {
      oran_buf_list_t *bufs = get_xran_buffers(ant_id / nb_tx_per_ru);
      const struct xran_frame_config *frame_conf = &get_xran_fh_config(ant_id / nb_tx_per_ru)->frame_conf;
      // skip processing this slot is TX (no TX in this slot)
      if (!is_tdd_dl_guard_slot(frame_conf, slot)) {
        continue;
      }
      // This loop would better be more inner to avoid confusion and maybe also errors.
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        /* skip UL and guard symbols. */
        if (!is_tdd_dl_symbol(frame_conf, slot, sym_idx)) {
          continue;
        }
        uint8_t *pData =
            bufs->src[ant_id % nb_tx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers[sym_idx % XRAN_NUM_OF_SYMBOL_PER_SLOT].pData;
        uint8_t *pPrbMapData = bufs->srccp[ant_id % nb_tx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
        struct xran_prb_map *pPrbMap = (struct xran_prb_map *)pPrbMapData;
        ptr = pData;
        pos = &ru->txdataF_BF[ant_id][sym_idx * fftsize];

        uint8_t *u8dptr;
        // even when the fragmentation occurs, nRBSize & nRBStart carry the same values in each prbMap
        // therefore, I took the liberty to just extract these values from the first prbMap
        struct xran_prb_elm *p_prbMapElm = &pPrbMap->prbMap[0];
        int num_totalRB = p_prbMapElm->nRBSize;
        int start_totalRB = p_prbMapElm->nRBStart;

        int pos_len = 0;
        int neg_len = 0;

        if (start_totalRB < (num_totalRB >> 1)) // there are PRBs left of DC
          neg_len = min((num_totalRB * 6) - (start_totalRB * 12), num_totalRB * N_SC_PER_PRB);
        pos_len = (num_totalRB * N_SC_PER_PRB) - neg_len;
        // Calculation of the pointer for the section in the buffer.
        // start of positive frequency component
        uint16_t *src1 = (uint16_t *)&pos[(neg_len == 0) ? ((start_totalRB * N_SC_PER_PRB) - (num_totalRB * 6)) : 0];
        // start of negative frequency component
        uint16_t *src2 = (uint16_t *)&pos[(start_totalRB * N_SC_PER_PRB) + fftsize - (num_totalRB * 6)];

        uint32_t local_src[num_totalRB * N_SC_PER_PRB] __attribute__((aligned(64)));
        memcpy((void *)local_src, (void *)src2, neg_len * 4);
        memcpy((void *)&local_src[neg_len], (void *)src1, pos_len * 4);
        if (ptr && pos) {
          u8dptr = (uint8_t *)ptr;
          int16_t payload_len = 0;

          uint8_t *dst = (uint8_t *)u8dptr;

          for (uint32_t idxElm = 0; idxElm < pPrbMap->nPrbElm; idxElm++) {
            struct xran_section_desc *p_sec_desc = NULL;
            struct xran_prb_elm *p_prbMapElm = &pPrbMap->prbMap[idxElm];
            if (sym_idx == 0) {
              // ant_id / no of antenna per beam gives the beam_nb
              p_prbMapElm->nBeamIndex = ru->beam_id[ant_id / (ru->nb_tx / ru->num_beams_period)][slot * XRAN_NUM_OF_SYMBOL_PER_SLOT];
              // In phy-f-1.0/fhi_lib/lib/api/xran_pkt_cp.h, beamId:15 is of 15bit. -1 set extension bit ef:1 to 1 mistakenly.
              if (p_prbMapElm->nBeamIndex == -1)
                p_prbMapElm->nBeamIndex = 0;
            }

            // radio-transport fragmentation is not supported in both E and F releases;
            // E-bit = 1 => each ethernet frame is considered as the last fragment;
            // a group of PRBs per each symbol is encapsulated in one ethernet frame.
            // => seems that the RUs don't check for E-bit
#ifdef E_RELEASE
            p_sec_desc = p_prbMapElm->p_sec_desc[sym_idx][0];
            int16_t startRB = p_prbMapElm->nRBStart;
            int16_t numRB = p_prbMapElm->nRBSize;
#elif F_RELEASE
            p_sec_desc = &p_prbMapElm->sec_desc[sym_idx][0];
            int16_t startRB = p_prbMapElm->UP_nRBStart;
            int16_t numRB = p_prbMapElm->UP_nRBSize;
#endif

            dst = xran_add_hdr_offset(dst, p_prbMapElm->compMethod);

            if (p_sec_desc == NULL) {
              printf("p_sec_desc == NULL\n");
              exit(-1);
            }
            uint16_t *dst16 = (uint16_t *)dst;

            if (p_prbMapElm->compMethod == XRAN_COMPMETHOD_NONE) {
              payload_len = numRB * N_SC_PER_PRB * 4L;
              /* convert to Network order */
              // NOTE: ggc 11 knows how to generate AVX2 for this!
              for (idx = 0; idx < (numRB * N_SC_PER_PRB) * 2; idx++)
                ((uint16_t *)dst16)[idx] = htons(((uint16_t *)local_src)[idx + startRB * N_SC_PER_PRB * 2]);
            } else if (p_prbMapElm->compMethod == XRAN_COMPMETHOD_BLKFLOAT) {
              payload_len = (3 * p_prbMapElm->iqWidth + 1) * numRB;

#if defined(__i386__) || defined(__x86_64__)
              struct xranlib_compress_request bfp_com_req = {};
              struct xranlib_compress_response bfp_com_rsp = {};

              uint32_t src_compr[num_totalRB * N_SC_PER_PRB] __attribute__((aligned(64)));
              if (numRB == num_totalRB) {
                bfp_com_req.data_in = (int16_t *)local_src;
              } else {
                memcpy(src_compr, local_src + (startRB * N_SC_PER_PRB), (numRB * N_SC_PER_PRB) * sizeof(*local_src));
                bfp_com_req.data_in = (int16_t *)src_compr;
              }

              bfp_com_req.numRBs = numRB;
              bfp_com_req.len = payload_len;
              bfp_com_req.compMethod = p_prbMapElm->compMethod;
              bfp_com_req.iqWidth = p_prbMapElm->iqWidth;

              bfp_com_rsp.data_out = (int8_t *)dst;
              bfp_com_rsp.len = 0;

              xranlib_compress_avx512(&bfp_com_req, &bfp_com_rsp);
#elif defined(__arm__) || defined(__aarch64__)
              armral_bfp_compression(p_prbMapElm->iqWidth, numRB, (int16_t *)local_src, (int8_t *)dst);
#else
              AssertFatal(1 == 0, "BFP compression not supported on this architecture");
#endif
            } else {
              printf("p_prbMapElm->compMethod == %d is not supported\n", p_prbMapElm->compMethod);
              exit(-1);
            }

            p_sec_desc->iq_buffer_offset = RTE_PTR_DIFF(dst, u8dptr);
            p_sec_desc->iq_buffer_len = payload_len;

            dst += payload_len;
            dst = xran_add_hdr_offset(dst, p_prbMapElm->compMethod);
          }

          // The tti should be updated as it increased.
          pPrbMap->tti_id = tti;

        } else {
          printf("ptr ==NULL\n");
          exit(-1); // fails here??
        }
      }
    }
  }
  return (0);
}

#ifdef F_RELEASE
/** @details Read PRACH and PUSCH data from xran buffers.  If
 * I/Q compression (bitwidth < 16 bits) is configured, deccompresses the data
 * before writing. Prints ON TIME counters every 128 frames.
 *
 * Function is blocking and waits for next frame/slot combination. It is unblocked
 * by oai_xran_fh_rx_callback(). It writes the current slot into parameters
 * frame/slot. */
int xran_fh_rx_read_slot_BySymbol(ru_info_t *ru, int *frame, int *slot)
{
  void *ptr = NULL;
  int32_t *pos = NULL;
  int idx = 0;

  static int64_t old_rx_counter[XRAN_PORTS_NUM] = {0};
  static int64_t old_tx_counter[XRAN_PORTS_NUM] = {0};
  struct xran_common_counters x_counters[XRAN_PORTS_NUM];
  static int outcnt = 0;
  #ifndef USE_POLLING
  // pull next even from oran_sync_fifo
  notifiedFIFO_elt_t *res = pullNotifiedFIFO(&oran_sync_fifo);

  notifiedFIFO_elt_t *f;
  while ((f = pollNotifiedFIFO(&oran_sync_fifo)) != NULL) {
    oran_sync_info_t *old_info = NotifiedFifoData(res);
    oran_sync_info_t *new_info = NotifiedFifoData(f);
    LOG_E(HW, "Detected double sync message %d.%d => %d.%d\n", old_info->f, old_info->sl, new_info->f, new_info->sl);
    delNotifiedFIFO_elt(res);
    res = f;
  }

  oran_sync_info_t *info = NotifiedFifoData(res);

  *slot = info->sl;
  *frame = info->f;
  delNotifiedFIFO_elt(res);
#else
  *slot = oran_sync_info.sl;
  *frame = oran_sync_info.f;
  uint32_t tti_in = oran_sync_info.tti;

  static int last_slot = -1;
  LOG_D(HW, "oran slot %d, last_slot %d\n", *slot, last_slot);
  int cnt = 0;
  // while (*slot == last_slot)  {
  while (tti_in == oran_sync_info.tti) {
    //*slot = oran_sync_info.sl;
    cnt++;
  }
  LOG_D(HW, "cnt %d, Reading %d.%d\n", cnt, *frame, *slot);
  last_slot = *slot;
#endif
  // return(0);

  struct xran_fh_config *fh_cfg = get_xran_fh_config(0);
  int slots_per_frame = 10 << fh_cfg->frame_conf.nNumerology;

  int tti = slots_per_frame * (*frame) + (*slot);

  read_prach_data(ru, *frame, *slot);

  const struct xran_fh_init *fh_init = get_xran_fh_init();
  int nPRBs = fh_cfg->nULRBs;
  int fftsize = 1 << fh_cfg->nULFftSize;

  int slot_offset_rxdata = 3 & (*slot);
  uint32_t slot_size = 4 * 14 * fftsize;
  uint8_t *rx_data = (uint8_t *)ru->rxdataF[0];
  uint8_t *start_ptr = NULL;
  int nb_rx_per_ru = ru->nb_rx / fh_init->xran_ports;
  for (uint16_t cc_id = 0; cc_id < 1 /*nSectorNum*/; cc_id++) { // OAI does not support multiple CC yet.
    for (uint8_t ant_id = 0; ant_id < ru->nb_rx; ant_id++) {
      rx_data = (uint8_t *)ru->rxdataF[ant_id];
      start_ptr = rx_data + (slot_size * slot_offset_rxdata);
      const struct xran_frame_config *frame_conf = &get_xran_fh_config(ant_id / nb_rx_per_ru)->frame_conf;
      // skip processing this slot is TX (no RX in this slot)
      if (!is_tdd_ul_guard_slot(frame_conf, *slot))
        continue;
      bool sym_start_found = false;
      int32_t sym_start = 0;
      // This loop would better be more inner to avoid confusion and maybe also errors.
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        /* the callback is for mixed and UL slots. In mixed, we have to
         * skip DL and guard symbols. */
        if (!is_tdd_ul_symbol(frame_conf, *slot, sym_idx))
          continue;
        if (!sym_start_found) {
          sym_start = sym_idx;
          sym_start_found = true;
        }

        uint8_t *pData;
        oran_buf_list_t *bufs = get_xran_buffers(ant_id / nb_rx_per_ru);
        uint8_t *pPrbMapData = bufs->dstcp[ant_id % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
        struct xran_prb_map *pPrbMap = (struct xran_prb_map *)pPrbMapData;

        struct xran_prb_map *pRbMap = pPrbMap;
        for (int idxElm = 0; idxElm < pRbMap->nPrbElm; idxElm++) {
          struct xran_prb_elm *pRbElm = &pRbMap->prbMap[idxElm];
#ifdef E_RELEASE
          struct xran_section_desc *p_sec_desc = pRbElm->p_sec_desc[sym_idx][0];
          uint32_t one_rb_size =
              (((pRbElm->iqWidth == 0) || (pRbElm->iqWidth == 16)) ? (N_SC_PER_PRB * 2 * 2) : (3 * pRbElm->iqWidth + 1));
          if (fh_init->mtu < pRbElm->nRBSize * one_rb_size)
            pData = bufs->dst[ant_id % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN]
                        .pBuffers[sym_idx % XRAN_NUM_OF_SYMBOL_PER_SLOT]
                        .pData;
          else
            pData = p_sec_desc->pData;
#elif defined F_RELEASE
          struct xran_section_desc *p_sec_desc = &pRbElm->sec_desc[sym_idx][0];
          pData = p_sec_desc->pData;
#endif 
          ptr = pData;
          pos = (int32_t *)(start_ptr + (4 * sym_idx * fftsize));
          if (ptr == NULL || pos == NULL)
            continue;

          uint8_t *src = (uint8_t *)ptr;

          LOG_D(HW, "rx pRbMap->nPrbElm %d\n", pRbMap->nPrbElm);
          // For Liteon FR2 with RunSlotPrbMapBySymbolEnable xran_prb_map will have xran_prb_elm prbMap[14], each idxElm matches to sym_idx.
          u_int8_t section_id_tmp = pPrbMap->nPrbElm < XRAN_NUM_OF_SYMBOL_PER_SLOT ? sym_idx - sym_start: sym_idx;
          if (section_id_tmp != pRbElm->nSectId) {
              LOG_D(HW,
                "rx prbMap[%d] : PRBstart %d nPRBs %d nSectId %d != sym_idx %d:%d\n",
                idxElm,
                pRbMap->prbMap[idxElm].nRBStart,
                pRbMap->prbMap[idxElm].nRBSize,
                pRbMap->prbMap[idxElm].nSectId, sym_idx, section_id_tmp
              );
            continue;
          }
          LOG_D(HW,
                "rx prbMap[%d] : PRBstart %d:%d nPRBs %d:%d nSectId %d sym_idx %d:%d\n",
                idxElm,
                pRbMap->prbMap[idxElm].nRBStart, pRbMap->prbMap[idxElm].UP_nRBStart,
                pRbMap->prbMap[idxElm].nRBSize, pRbMap->prbMap[idxElm].UP_nRBSize,
                pRbMap->prbMap[idxElm].nSectId, sym_idx, section_id_tmp
              );

          int pos_len = 0;
          int neg_len = 0;
          int num_prbu = p_sec_desc->num_prbu;
          int start_prbu = p_sec_desc->start_prbu;
          if (start_prbu < (nPRBs >> 1)) // there are PRBs left of DC
            neg_len = min((nPRBs * 6) - (start_prbu * 12), num_prbu * N_SC_PER_PRB);
          pos_len = (num_prbu * N_SC_PER_PRB) - neg_len;

          src = pData;
          // Calculation of the pointer for the section in the buffer.
          // positive half
          uint8_t *dst1 = (uint8_t *)(pos + (neg_len == 0 ? ((start_prbu * N_SC_PER_PRB) - (nPRBs * 6)) : 0));
          // negative half
          uint8_t *dst2 = (uint8_t *)(pos + (start_prbu * N_SC_PER_PRB) + fftsize - (nPRBs * 6));
          int32_t local_dst[num_prbu * N_SC_PER_PRB] __attribute__((aligned(64)));
          if (pRbElm->compMethod == XRAN_COMPMETHOD_NONE) {
            // NOTE: gcc 11 knows how to generate AVX2 for this!
            for (idx = 0; idx < num_prbu * N_SC_PER_PRB * 2; idx++)
              ((int16_t *)local_dst)[idx] = ((int16_t)ntohs(((uint16_t *)src)[idx])) >> 2;
            memcpy((void *)dst2, (void *)local_dst, neg_len * 4);
            memcpy((void *)dst1, (void *)&local_dst[neg_len], pos_len * 4);
          } else if (pRbElm->compMethod == XRAN_COMPMETHOD_BLKFLOAT) {
#if defined(__i386__) || defined(__x86_64__)
            struct xranlib_decompress_request bfp_decom_req = {};
            struct xranlib_decompress_response bfp_decom_rsp = {};

            int16_t payload_len = (3 * pRbElm->iqWidth + 1) * num_prbu;

            bfp_decom_req.data_in = (int8_t *)src;
            bfp_decom_req.numRBs = num_prbu;
            bfp_decom_req.len = payload_len;
            bfp_decom_req.compMethod = pRbElm->compMethod;
            bfp_decom_req.iqWidth = pRbElm->iqWidth;

            bfp_decom_rsp.data_out = (int16_t *)local_dst;
            bfp_decom_rsp.len = 0;

            xranlib_decompress_avx512(&bfp_decom_req, &bfp_decom_rsp);
#elif defined(__arm__) || defined(__aarch64__)
            armral_bfp_decompression(pRbElm->iqWidth, num_prbu, (int8_t *)src, (int16_t *)local_dst);
#else
            AssertFatal(1 == 0, "BFP compression not supported on this architecture");
#endif
            memcpy((void *)dst2, (void *)local_dst, neg_len * 4);
            memcpy((void *)dst1, (void *)&local_dst[neg_len], pos_len * 4);
            outcnt++;
          } else {
            printf("pRbElm->compMethod == %d is not supported\n", pRbElm->compMethod);
            exit(-1);
          }
        }
      } // sym_ind
    } // ant_ind
  } // vv_inf
  if ((*frame & 0x7f) == 0 && *slot == 0 && xran_get_common_counters(gxran_handle, &x_counters[0]) == XRAN_STATUS_SUCCESS) {
    for (int o_xu_id = 0; o_xu_id < fh_init->xran_ports; o_xu_id++) {
      LOG_I(HW,
            "[%s%d][rx %7ld pps %7ld kbps %7ld][tx %7ld pps %7ld kbps %7ld][Total Msgs_Rcvd %ld]\n",
            "o-du ",
            o_xu_id,
            x_counters[o_xu_id].rx_counter,
            x_counters[o_xu_id].rx_counter - old_rx_counter[o_xu_id],
            x_counters[o_xu_id].rx_bytes_per_sec * 8 / 1000L,
            x_counters[o_xu_id].tx_counter,
            x_counters[o_xu_id].tx_counter - old_tx_counter[o_xu_id],
            x_counters[o_xu_id].tx_bytes_per_sec * 8 / 1000L,
            x_counters[o_xu_id].Total_msgs_rcvd);
      for (int rxant = 0; rxant < ru->nb_rx / fh_init->xran_ports; rxant++)
        LOG_I(HW,
              "[%s%d][pusch%d %7ld prach%d %7ld]\n",
              "o_du",
              o_xu_id,
              rxant,
              x_counters[o_xu_id].rx_pusch_packets[rxant],
              rxant,
              x_counters[o_xu_id].rx_prach_packets[rxant]);
      if (x_counters[o_xu_id].rx_counter > old_rx_counter[o_xu_id])
        old_rx_counter[o_xu_id] = x_counters[o_xu_id].rx_counter;
      if (x_counters[o_xu_id].tx_counter > old_tx_counter[o_xu_id])
        old_tx_counter[o_xu_id] = x_counters[o_xu_id].tx_counter;
    }
  }
  return (0);
}

/** @details Write PDSCH IQ-data from OAI txdataF_BF buffer to xran buffers. If
 * I/Q compression (bitwidth < 16 bits) is configured, compresses the data
 * before writing. */
int xran_fh_tx_send_slot_BySymbol(ru_info_t *ru, int frame, int slot, uint64_t timestamp)
{
  int tti = /*frame*SUBFRAMES_PER_SYSTEMFRAME*SLOTNUM_PER_SUBFRAME+*/ 20 * frame
            + slot; // commented out temporarily to check that compilation of oran 5g is working.

  void *ptr = NULL;
  int32_t *pos = NULL;
  int idx = 0;

  const struct xran_fh_init *fh_init = get_xran_fh_init();
  const struct xran_fh_config *fh_cfg = get_xran_fh_config(0);
  int nPRBs = fh_cfg->nDLRBs;
  int fftsize = 1 << fh_cfg->nDLFftSize;
  int nb_tx_per_ru = ru->nb_tx / fh_init->xran_ports;
  int nb_rx_per_ru = ru->nb_rx / fh_init->xran_ports;

  // Handle CP UL packet here instead of at xran_fh_rx_read_slot() as oran_fh_if4p5_south_in() lags behind
  // oran_fh_if4p5_south_out() (which is invoked at the right time slot) by 4 slots.
  // Need to use --continuous-tx so that this routine will be triggered in RX slot.
  for (uint16_t cc_id = 0; cc_id < 1 /*nSectorNum*/; cc_id++) { // OAI does not support multiple CC yet.
    for (uint8_t ant_id = 0; ant_id < ru->nb_rx; ant_id++) {
      const struct xran_frame_config *frame_conf = &get_xran_fh_config(ant_id / nb_rx_per_ru)->frame_conf;
      // skip processing this slot is TX (no RX in this slot)
      if (!is_tdd_ul_guard_slot(frame_conf, slot)) {
        continue;
      }
      bool sym_start_found = false;
      int32_t sym_start = 0;
      // This loop would better be more inner to avoid confusion and maybe also errors.
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        /* the callback is for mixed and UL slots. In mixed, we have to
         * skip DL and guard symbols. */
        if (!is_tdd_ul_symbol(frame_conf, slot, sym_idx)) {
          continue;
        }
        if (!sym_start_found) {
          sym_start = sym_idx;
          sym_start_found = true;
        }

        oran_buf_list_t *bufs = get_xran_buffers(ant_id / nb_rx_per_ru);
        uint8_t *pPrbMapData = bufs->dstcp[ant_id % nb_rx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
        struct xran_prb_map *pPrbMap = (struct xran_prb_map *)pPrbMapData;

        struct xran_prb_elm *pRbElm = &pPrbMap->prbMap[0];

        struct xran_prb_map *pRbMap = pPrbMap;
        uint32_t idxElm = 0;

        LOG_D(HW, "tx0 pRbMap->nPrbElm %d\n", pRbMap->nPrbElm);
        for (idxElm = 0; idxElm < pRbMap->nPrbElm; idxElm++) {
          LOG_D(HW, "prbMap[%d] : PRBstart %d nPRBs %d\n", idxElm, pRbMap->prbMap[idxElm].nRBStart, pRbMap->prbMap[idxElm].nRBSize);
          pRbElm = &pRbMap->prbMap[idxElm];
          // For Liteon FR2 with RunSlotPrbMapBySymbolEnable xran_prb_map will have xran_prb_elm prbMap[14], each idxElm matches to sym_idx.
          u_int8_t section_id_tmp = pPrbMap->nPrbElm < XRAN_NUM_OF_SYMBOL_PER_SLOT ? sym_idx - sym_start: sym_idx;
          if (section_id_tmp != pRbElm->nSectId) {
              LOG_D(HW,
                    "tx0 prbMap[%d] : PRBstart %d:%d nPRBs %d:%d nSectId %d != sym_idx %d:%d\n",
                    idxElm,
                    pRbMap->prbMap[idxElm].nRBStart, pRbMap->prbMap[idxElm].UP_nRBStart,
                    pRbMap->prbMap[idxElm].nRBSize, pRbMap->prbMap[idxElm].UP_nRBSize,
                    pRbMap->prbMap[idxElm].nSectId, sym_idx, section_id_tmp
                    );
            continue;
          }
          LOG_D(HW,
                "tx0 prbMap[%d] : PRBstart %d:%d nPRBs %d:%d nSectId %d sym_idx %d:%d\n",
                idxElm,
                pRbMap->prbMap[idxElm].nRBStart, pRbMap->prbMap[idxElm].UP_nRBStart,
                pRbMap->prbMap[idxElm].nRBSize, pRbMap->prbMap[idxElm].UP_nRBSize,
                pRbMap->prbMap[idxElm].nSectId, sym_idx, section_id_tmp
                );

          // ant_id / no of antenna per beam gives the beam_nb
          pRbElm->nBeamIndex = ru->beam_id[ant_id / (ru->nb_rx / ru->num_beams_period)][slot * XRAN_NUM_OF_SYMBOL_PER_SLOT + sym_idx];
          // In phy-f-1.0/fhi_lib/lib/api/xran_pkt_cp.h, beamId:15 is of 15bit. -1 set extension bit ef:1 to 1 mistakenly.
          if (pRbElm->nBeamIndex == -1)
            pRbElm->nBeamIndex = 0;
        }
      }
    }
  }

  for (uint16_t cc_id = 0; cc_id < 1 /*nSectorNum*/; cc_id++) { // OAI does not support multiple CC yet.
    for (uint8_t ant_id = 0; ant_id < ru->nb_tx; ant_id++) {
      oran_buf_list_t *bufs = get_xran_buffers(ant_id / nb_tx_per_ru);
      const struct xran_frame_config *frame_conf = &get_xran_fh_config(ant_id / nb_tx_per_ru)->frame_conf;
      // skip processing this slot is TX (no TX in this slot)
      if (!is_tdd_dl_guard_slot(frame_conf, slot)) {
        continue;
      }

      // Set nPrbElm if beam_id = -1 for all downlink symbols
      bool beam_used = false;
      uint8_t *pPrbMapData = bufs->srccp[ant_id % nb_tx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
      struct xran_prb_map *pPrbMap = (struct xran_prb_map *)pPrbMapData;
      struct xran_prb_map *pRbMap = pPrbMap;
      int32_t dl_sym_end = 0;
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        if (is_tdd_dl_symbol(frame_conf, slot, sym_idx)) {
          if (ru->beam_id[ant_id / (ru->nb_tx / ru->num_beams_period)][slot * XRAN_NUM_OF_SYMBOL_PER_SLOT+ sym_idx] != -1)
            beam_used |= true;
        }
        else {
            dl_sym_end = sym_idx;
            break;
        }
      }
      if (is_tdd_guard_slot(frame_conf, slot))
        pRbMap->nPrbElm = dl_sym_end;
      else
        pRbMap->nPrbElm = XRAN_NUM_OF_SYMBOL_PER_SLOT;
      if (!beam_used) {
        pRbMap->nPrbElm = 0;
        continue;
      }

      // This loop would better be more inner to avoid confusion and maybe also errors.
      for (int32_t sym_idx = 0; sym_idx < XRAN_NUM_OF_SYMBOL_PER_SLOT; sym_idx++) {
        /* the callback is for mixed and UL slots. In mixed, we have to
         * skip UL and guard symbols. */
        if (is_tdd_ul_symbol(frame_conf, slot, sym_idx)) {
          continue;
        }
        uint8_t *pData =
            bufs->src[ant_id % nb_tx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers[sym_idx % XRAN_NUM_OF_SYMBOL_PER_SLOT].pData;
        uint8_t *pPrbMapData = bufs->srccp[ant_id % nb_tx_per_ru][tti % XRAN_N_FE_BUF_LEN].pBuffers->pData;
        struct xran_prb_map *pPrbMap = (struct xran_prb_map *)pPrbMapData;
        ptr = pData;
        pos = &ru->txdataF_BF[ant_id][sym_idx * fftsize];

        uint8_t *u8dptr;
        struct xran_prb_map *pRbMap = pPrbMap;
        int32_t sym_id = sym_idx % XRAN_NUM_OF_SYMBOL_PER_SLOT;
        if (ptr && pos) {
          uint32_t idxElm = 0;
          u8dptr = (uint8_t *)ptr;
          int16_t payload_len = 0;

          uint8_t *dst = (uint8_t *)u8dptr;

          struct xran_prb_elm *p_prbMapElm = &pRbMap->prbMap[idxElm];
          LOG_D(HW, "tx1 pRbMap->nPrbElm %d\n", pRbMap->nPrbElm);
          for (idxElm = 0; idxElm < pRbMap->nPrbElm; idxElm++) {
            // For Liteon FR2 with RunSlotPrbMapBySymbolEnable xran_prb_map will have xran_prb_elm prbMap[14], each idxElm matches to sym_idx.
            struct xran_section_desc *p_sec_desc = NULL;
            p_prbMapElm = &pRbMap->prbMap[idxElm];
            if (sym_idx != p_prbMapElm->nSectId)
              continue;
            // ant_id / no of antenna per beam gives the beam_nb
            p_prbMapElm->nBeamIndex = ru->beam_id[ant_id / (ru->nb_tx / ru->num_beams_period)][slot * XRAN_NUM_OF_SYMBOL_PER_SLOT+ sym_idx];
            // In phy-f-1.0/fhi_lib/lib/api/xran_pkt_cp.h, beamId:15 is of 15bit. -1 set extension bit ef:1 to 1 mistakenly.
            if (p_prbMapElm->nBeamIndex == -1)
              p_prbMapElm->nBeamIndex = 0;

            // assumes one fragment per symbol
#ifdef E_RELEASE
            p_sec_desc = p_prbMapElm->p_sec_desc[sym_id][0];
#elif F_RELEASE
            p_sec_desc = &p_prbMapElm->sec_desc[sym_id][0];
#endif

            dst = xran_add_hdr_offset(dst, p_prbMapElm->compMethod);

            if (p_sec_desc == NULL) {
              printf("p_sec_desc == NULL\n");
              exit(-1);
            }
            uint16_t *dst16 = (uint16_t *)dst;

            int pos_len = 0;
            int neg_len = 0;

            if (p_prbMapElm->UP_nRBStart < (nPRBs >> 1)) // there are PRBs left of DC
              neg_len = min((nPRBs * 6) - (p_prbMapElm->UP_nRBStart * 12), p_prbMapElm->UP_nRBSize * N_SC_PER_PRB);
            pos_len = (p_prbMapElm->UP_nRBSize * N_SC_PER_PRB) - neg_len;
            // Calculation of the pointer for the section in the buffer.
            // start of positive frequency component
            uint16_t *src1 = (uint16_t *)&pos[(neg_len == 0) ? ((p_prbMapElm->UP_nRBStart * N_SC_PER_PRB) - (nPRBs * 6)) : 0];
            // start of negative frequency component
            uint16_t *src2 = (uint16_t *)&pos[(p_prbMapElm->UP_nRBStart * N_SC_PER_PRB) + fftsize - (nPRBs * 6)];

            uint32_t local_src[p_prbMapElm->UP_nRBSize * N_SC_PER_PRB] __attribute__((aligned(64)));
            memcpy((void *)local_src, (void *)src2, neg_len * 4);
            memcpy((void *)&local_src[neg_len], (void *)src1, pos_len * 4);
            if (p_prbMapElm->compMethod == XRAN_COMPMETHOD_NONE) {
              payload_len = p_prbMapElm->UP_nRBSize * N_SC_PER_PRB * 4L;
              /* convert to Network order */
              // NOTE: ggc 11 knows how to generate AVX2 for this!
              for (idx = 0; idx < (pos_len + neg_len) * 2; idx++)
                ((uint16_t *)dst16)[idx] = htons(((uint16_t *)local_src)[idx]);
            } else if (p_prbMapElm->compMethod == XRAN_COMPMETHOD_BLKFLOAT) {
              payload_len = (3 * p_prbMapElm->iqWidth + 1) * p_prbMapElm->UP_nRBSize;

#if defined(__i386__) || defined(__x86_64__)
              struct xranlib_compress_request bfp_com_req = {};
              struct xranlib_compress_response bfp_com_rsp = {};

              bfp_com_req.data_in = (int16_t *)local_src;
              bfp_com_req.numRBs = p_prbMapElm->UP_nRBSize;
              bfp_com_req.len = payload_len;
              bfp_com_req.compMethod = p_prbMapElm->compMethod;
              bfp_com_req.iqWidth = p_prbMapElm->iqWidth;

              bfp_com_rsp.data_out = (int8_t *)dst;
              bfp_com_rsp.len = 0;

              xranlib_compress_avx512(&bfp_com_req, &bfp_com_rsp);
#elif defined(__arm__) || defined(__aarch64__)
              armral_bfp_compression(p_prbMapElm->iqWidth, p_prbMapElm->UP_nRBSize, (int16_t *)local_src, (int8_t *)dst);
#else
              AssertFatal(1 == 0, "BFP compression not supported on this architecture");
#endif

            } else {
              printf("p_prbMapElm->compMethod == %d is not supported\n", p_prbMapElm->compMethod);
              exit(-1);
            }

            p_sec_desc->iq_buffer_offset = RTE_PTR_DIFF(dst, u8dptr);
            p_sec_desc->iq_buffer_len = payload_len;

            dst += payload_len;
            dst = xran_add_hdr_offset(dst, p_prbMapElm->compMethod);
          }

          // The tti should be updated as it increased.
          pRbMap->tti_id = tti;

        } else {
          printf("ptr ==NULL\n");
          exit(-1); // fails here??
        }
      }
    }
  }
  return (0);
}
#endif