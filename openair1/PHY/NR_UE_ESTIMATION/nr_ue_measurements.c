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

/*! \file nr_ue_measurements.c
 * \brief UE measurements routines
 * \author  R. Knopp, G. Casati, K. Saaifan
 * \date 2020
 * \version 0.1
 * \company Eurecom, Fraunhofer IIS
 * \email: knopp@eurecom.fr, guido.casati@iis.fraunhofer.de, khodr.saaifan@iis.fraunhofer.de
 * \note
 * \warning
 */

#include "executables/softmodem-common.h"
#include "executables/nr-softmodem-common.h"
#include "PHY/defs_nr_UE.h"
#include "PHY/INIT/nr_phy_init.h"
#include "common/utils/LOG/log.h"
#include "PHY/sse_intrin.h"
#include "SCHED_NR_UE/defs.h"
#include "PHY/NR_REFSIG/sss_nr.h"
#include "PHY/NR_REFSIG/pss_nr.h"
#include "PHY/NR_REFSIG/ss_pbch_nr.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"

#define K1 ((long long int) 512)
#define K2 ((long long int) (1024-K1))

//#define DEBUG_MEAS_RRC
//#define DEBUG_MEAS_UE
//#define DEBUG_RANK_EST

extern openair0_config_t openair0_cfg[MAX_CARDS];

void nr_ue_measurements(PHY_VARS_NR_UE *ue,
                        const UE_nr_rxtx_proc_t *proc,
                        int number_rbs,
                        uint32_t pdsch_est_size,
                        int32_t dl_ch_estimates[][pdsch_est_size])
{
  int slot = proc->nr_slot_rx;
  int aarx, aatx, gNB_id = 0;
  NR_DL_FRAME_PARMS *frame_parms = &ue->frame_parms;
  int ch_offset = frame_parms->ofdm_symbol_size*2;
  ue->measurements.nb_antennas_rx = frame_parms->nb_antennas_rx;

  allocCast3D(rx_spatial_power,
              int,
              ue->measurements.rx_spatial_power,
              NUMBER_OF_CONNECTED_gNB_MAX,
              cmax(frame_parms->nb_antenna_ports_gNB, 1),
              cmax(frame_parms->nb_antennas_rx, 1),
              false);
  allocCast3D(rx_spatial_power_dB,
              unsigned short,
              ue->measurements.rx_spatial_power_dB,
              NUMBER_OF_CONNECTED_gNB_MAX,
              cmax(frame_parms->nb_antenna_ports_gNB, 1),
              cmax(frame_parms->nb_antennas_rx, 1),
              false);

  // signal measurements
  for (gNB_id = 0; gNB_id < ue->n_connected_gNB; gNB_id++){

    ue->measurements.rx_power_tot[gNB_id] = 0;

    for (aarx = 0; aarx < frame_parms->nb_antennas_rx; aarx++){

      ue->measurements.rx_power[gNB_id][aarx] = 0;

      for (aatx = 0; aatx < frame_parms->nb_antenna_ports_gNB; aatx++){
        const int z = signal_energy_nodc((c16_t*)&dl_ch_estimates[gNB_id][ch_offset], number_rbs * NR_NB_SC_PER_RB);
        rx_spatial_power[gNB_id][aatx][aarx] = z;

        if (rx_spatial_power[gNB_id][aatx][aarx] < 0)
          rx_spatial_power[gNB_id][aatx][aarx] = 0;

        rx_spatial_power_dB[gNB_id][aatx][aarx] = (unsigned short)dB_fixed(rx_spatial_power[gNB_id][aatx][aarx]);
        ue->measurements.rx_power[gNB_id][aarx] += rx_spatial_power[gNB_id][aatx][aarx];
      }

      ue->measurements.rx_power_dB[gNB_id][aarx] = (unsigned short) dB_fixed(ue->measurements.rx_power[gNB_id][aarx]);
      ue->measurements.rx_power_tot[gNB_id] += ue->measurements.rx_power[gNB_id][aarx];

    }

    ue->measurements.rx_power_tot_dB[gNB_id] = (unsigned short) dB_fixed(ue->measurements.rx_power_tot[gNB_id]);

  }

  // filter to remove jitter
  if (ue->init_averaging == 0) {

    for (gNB_id = 0; gNB_id < ue->n_connected_gNB; gNB_id++)
      ue->measurements.rx_power_avg[gNB_id] = (int)((K1 * ue->measurements.rx_power_avg[gNB_id] + K2 * ue->measurements.rx_power_tot[gNB_id]) >> 10);

    ue->measurements.n0_power_avg = (int)((K1 * ue->measurements.n0_power_avg + K2 * ue->measurements.n0_power_tot) >> 10);

    LOG_D(PHY, "Noise Power Computation: K1 %lld K2 %lld n0 avg %u n0 tot %u\n", K1, K2, ue->measurements.n0_power_avg, ue->measurements.n0_power_tot);

  } else {

    for (gNB_id = 0; gNB_id < ue->n_connected_gNB; gNB_id++)
      ue->measurements.rx_power_avg[gNB_id] = ue->measurements.rx_power_tot[gNB_id];

    ue->measurements.n0_power_avg = ue->measurements.n0_power_tot;
    ue->init_averaging = 0;

  }

  for (gNB_id = 0; gNB_id < ue->n_connected_gNB; gNB_id++) {

    ue->measurements.rx_power_avg_dB[gNB_id] = dB_fixed( ue->measurements.rx_power_avg[gNB_id]);
    ue->measurements.n0_power_avg_dB = dB_fixed(ue->measurements.n0_power_avg);
    ue->measurements.wideband_cqi_tot[gNB_id] = ue->measurements.rx_power_tot_dB[gNB_id] - ue->measurements.n0_power_tot_dB;
    ue->measurements.wideband_cqi_avg[gNB_id] = ue->measurements.rx_power_avg_dB[gNB_id] - ue->measurements.n0_power_avg_dB;
    ue->measurements.rx_rssi_dBm[gNB_id] =
        ue->measurements.rx_power_avg_dB[gNB_id] + 30 - SQ15_SQUARED_NORM_FACTOR_DB
        - ((int)openair0_cfg[ue->rf_map.card].rx_gain[0] - (int)openair0_cfg[ue->rf_map.card].rx_gain_offset[0])
        - dB_fixed(ue->frame_parms.ofdm_symbol_size);

    LOG_D(PHY, "[gNB %d] Slot %d, RSSI %d dB (%d dBm/RE), WBandCQI %d dB, rxPwrAvg %d, n0PwrAvg %d\n",
      gNB_id,
      slot,
      ue->measurements.rx_power_avg_dB[gNB_id],
      ue->measurements.rx_rssi_dBm[gNB_id],
      ue->measurements.wideband_cqi_avg[gNB_id],
      ue->measurements.rx_power_avg[gNB_id],
      ue->measurements.n0_power_tot);
  }
}

// This function calculates:
// - SS reference signal received digital power in dB/RE
uint32_t nr_ue_calculate_ssb_rsrp(const NR_DL_FRAME_PARMS *fp,
                                  const UE_nr_rxtx_proc_t *proc,
                                  const c16_t rxdataF[][fp->samples_per_slot_wCP],
                                  int symbol_offset,
                                  int ssb_start_subcarrier)
{
  int k_start = 56;
  int k_end   = 183;
  unsigned int ssb_offset = fp->first_carrier_offset + ssb_start_subcarrier;

  uint8_t l_sss = (symbol_offset + 2) % fp->symbols_per_slot;

  uint32_t rsrp = 0;

  LOG_D(PHY, "In %s: l_sss %d ssb_offset %d\n", __FUNCTION__, l_sss, ssb_offset);
  int nb_re = 0;

  for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
    int16_t *rxF_sss = (int16_t *)&rxdataF[aarx][l_sss * fp->ofdm_symbol_size];

    for(int k = k_start; k < k_end; k++){
      int re = (ssb_offset + k) % fp->ofdm_symbol_size;

#ifdef DEBUG_MEAS_UE
      LOG_I(PHY, "In %s rxF_sss[%d] %d %d\n", __FUNCTION__, re, rxF_sss[re * 2], rxF_sss[re * 2 + 1]);
#endif

      rsrp += (((int32_t)rxF_sss[re*2]*rxF_sss[re*2]) + ((int32_t)rxF_sss[re*2 + 1]*rxF_sss[re*2 + 1]));
      nb_re++;

    }
  }

  rsrp /= nb_re;

  LOG_D(PHY, "In %s: RSRP/nb_re: %d nb_re :%d\n", __FUNCTION__, rsrp, nb_re);

  return rsrp;
}

// Send SSB RSRP measurement to MAC
static void send_ssb_rsrp_meas(PHY_VARS_NR_UE *ue,
                               const UE_nr_rxtx_proc_t *proc,
                               uint16_t Nid_cell,
                               int rsrp_dBm,
                               bool is_neighboring_cell,
                               int ssb_index,
                               float sinr_dB)
{
  if (!ue->if_inst || !ue->if_inst->dl_indication)
    return;

  fapi_nr_l1_measurements_t l1_measurements = {
      .gNB_index = proc->gNB_id,
      .meas_type = NFAPI_NR_SS_MEAS,
      .Nid_cell = Nid_cell,
      .is_neighboring_cell = is_neighboring_cell,
      .rsrp_dBm = rsrp_dBm,
      .ssb_index = ssb_index,
      .sinr_dB = sinr_dB,
  };

  nr_downlink_indication_t dl_indication = {0};
  fapi_nr_rx_indication_t rx_ind = {0};
  nr_fill_dl_indication(&dl_indication, NULL, &rx_ind, proc, ue, NULL);
  nr_fill_rx_indication(&rx_ind, FAPI_NR_MEAS_IND, ue, NULL, NULL, 1, proc, &l1_measurements, NULL);
  ue->if_inst->dl_indication(&dl_indication);
}

// This function implements:
// - SS reference signal received power (SS-RSRP) as per clause 5.1.1 of 3GPP TS 38.215 version 16.3.0 Release 16
// - no Layer 3 filtering implemented (no filterCoefficient provided from RRC)
// Todo:
// - Layer 3 filtering according to clause 5.5.3.2 of 3GPP TS 38.331 version 16.2.0 Release 16
// Measurement units:
// - RSRP:    W (dBW)
// - RX Gain  dB
void nr_ue_ssb_rsrp_measurements(PHY_VARS_NR_UE *ue,
                                 int ssb_index,
                                 const UE_nr_rxtx_proc_t *proc,
                                 c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;

  int symbol_offset = nr_get_ssb_start_symbol(fp, ssb_index);

  if (fp->half_frame_bit)
    symbol_offset += (fp->slots_per_frame >> 1) * fp->symbols_per_slot;

  uint32_t rsrp_avg = nr_ue_calculate_ssb_rsrp(fp, proc, rxdataF, symbol_offset, fp->ssb_start_subcarrier);
  float rsrp_db_per_re = 10 * log10(rsrp_avg);

  openair0_config_t *cfg0 = &openair0_cfg[ue->rf_map.card];

  if (rsrp_avg == 0)
    ue->measurements.ssb_rsrp_dBm[ssb_index] = -200; // lower than any value to be reported per Table 10.1.6.1-1 of 38.133
  else
    ue->measurements.ssb_rsrp_dBm[ssb_index] = rsrp_db_per_re + 30 - SQ15_SQUARED_NORM_FACTOR_DB
                                               - ((int)cfg0->rx_gain[0] - (int)cfg0->rx_gain_offset[0])
                                               - dB_fixed(fp->ofdm_symbol_size);

  // to obtain non-integer dB value with a resoluion of 0.5dB
  uint32_t signal_pwr = rsrp_avg > ue->measurements.n0_power_avg ? rsrp_avg - ue->measurements.n0_power_avg : 0;
  int SNRtimes10 = dB_fixed_x10(signal_pwr) - dB_fixed_x10(ue->measurements.n0_power_avg);
  ue->measurements.ssb_sinr_dB[ssb_index] = SNRtimes10 / 10.0;

  LOG_D(PHY,
        "[UE %d] ssb %d SS-RSRP: %d dBm/RE (%f dB/RE), SS-SINR: %f dB\n",
        ue->Mod_id,
        ssb_index,
        ue->measurements.ssb_rsrp_dBm[ssb_index],
        rsrp_db_per_re,
        ue->measurements.ssb_sinr_dB[ssb_index]);

  // Send SS measurements to MAC
  send_ssb_rsrp_meas(ue,
                     proc,
                     ue->frame_parms.Nid_cell,
                     ue->measurements.ssb_rsrp_dBm[ssb_index],
                     false,
                     ssb_index,
                     ue->measurements.ssb_sinr_dB[ssb_index]);
}

static bool search_neighboring_cell(NR_DL_FRAME_PARMS *frame_parms,
                                    fapi_nr_neighboring_cell_t *nr_neighboring_cell,
                                    neighboring_cell_info_t *neighboring_cell_info,
                                    c16_t **rxdata,
                                    uint32_t rxdata_size,
                                    uint32_t rxdataF_sz,
                                    c16_t rxdataF[][rxdataF_sz],
                                    c16_t pssTime[][frame_parms->ofdm_symbol_size])
{
  int detected_nid_cell = -1;
  int ssb_offset = 0;
  int freq_offset_pss = 0;
  int pss_peak = 0;
  int pss_avg = 0;
  int32_t sss_metric = 0;
  uint8_t sss_phase = 0;
  int freq_offset_sss = 0;

  nr_ssb_search_params_t search_params = {
      .frame_parms = frame_parms,
      .rxdata = rxdata,
      .rxdata_size = rxdata_size,
      .ssb_start_subcarrier = frame_parms->ssb_start_subcarrier,
      .target_nid_cell = -1, // Blind search
      .exclude_nid_cell = frame_parms->Nid_cell, // Exclude serving cell
      .apply_freq_offset = false,
      .search_frame_id = 0, // Search in first frame of buffer
      .fo_flag = false,
      .rxdataF = rxdataF,
      .pssTime = pssTime,
      .detected_nid_cell = &detected_nid_cell,
      .ssb_offset = &ssb_offset,
      .sss_metric = &sss_metric,
      .freq_offset_pss = &freq_offset_pss,
      .freq_offset_sss = &freq_offset_sss,
      .sss_phase = &sss_phase,
      .pss_peak = &pss_peak,
      .pss_avg = &pss_avg,
  };

  bool found = nr_search_ssb_common(&search_params);

  if (found) {
    nr_neighboring_cell->Nid_cell = detected_nid_cell;
    LOG_I(NR_PHY,
          "Found neighbor cell PCI=%d (sss_metric=%d, ssb_offset=%d, pss_peak=%d dB, pss_avg=%d dB)\n",
          detected_nid_cell,
          sss_metric,
          ssb_offset,
          pss_peak,
          pss_avg);

    // Update search window
    neighboring_cell_info->pss_search_start = ssb_offset + frame_parms->nb_prefix_samples - 16;
    neighboring_cell_info->pss_search_length = 32;
  }

  return found;
}

static bool validate_known_pci(NR_DL_FRAME_PARMS *frame_parms,
                               fapi_nr_neighboring_cell_t *nr_neighboring_cell,
                               neighboring_cell_info_t *neighboring_cell_info,
                               c16_t **rxdata,
                               uint32_t rxdataF_sz,
                               c16_t rxdataF[][rxdataF_sz],
                               c16_t pssTime[][frame_parms->ofdm_symbol_size])
{
  int known_pci = nr_neighboring_cell->Nid_cell;
  int pss_index = GET_NID2(known_pci);
  int f_off = 0;
  int pss_peak = 0;
  int pss_avg = 0;

  int start = neighboring_cell_info->pss_search_start;
  int length = neighboring_cell_info->pss_search_length;

  int peak_position = pss_search_time_nr((const c16_t **)rxdata,
                                         frame_parms,
                                         pssTime,
                                         false, // no frequency offset estimation for tracking
                                         0, // first frame
                                         known_pci,
                                         &pss_index,
                                         &f_off,
                                         &pss_peak,
                                         &pss_avg,
                                         start,
                                         length);

  if (peak_position < frame_parms->nb_prefix_samples) {
    if (neighboring_cell_info->valid_meas)
      neighboring_cell_info->consec_fail++;
    LOG_D(NR_PHY,
          "PSS validation failed for PCI=%d (search window: start=%d, length=%d, peak=%d dB, avg=%d dB), consec_fail=%d\n",
          known_pci,
          start,
          length,
          pss_peak,
          pss_avg,
          neighboring_cell_info->consec_fail);
    return false;
  }

  int ssb_offset = peak_position - frame_parms->nb_prefix_samples;

  uint8_t sss_symbol = SSS_SYMBOL_NB - PSS_SYMBOL_NB;
  nr_slot_fep(NULL, frame_parms, 0, 0, rxdataF, link_type_dl, ssb_offset, (c16_t **)rxdata);
  nr_slot_fep(NULL, frame_parms, 0, sss_symbol, rxdataF, link_type_dl, ssb_offset, (c16_t **)rxdata);

  int detected_nid_cell = -1;
  int32_t sss_metric = 0;
  uint8_t sss_phase = 0;
  int freq_offset_sss = 0;
  bool sss_detected = rx_sss_nr(frame_parms,
                                pss_index,
                                known_pci,
                                0,
                                frame_parms->ssb_start_subcarrier,
                                &detected_nid_cell,
                                &sss_metric,
                                &sss_phase,
                                &freq_offset_sss,
                                rxdataF);

  if (!sss_detected) {
    if (neighboring_cell_info->valid_meas)
      neighboring_cell_info->consec_fail++;
    LOG_D(NR_PHY,
          "Known PCI validation failed for PCI=%d (metric=%d), consec_fail=%d\n",
          known_pci,
          sss_metric,
          neighboring_cell_info->consec_fail);
    return false;
  }

  LOG_D(NR_PHY, "Known PCI validation completed for PCI=%d, metric=%d\n", known_pci, sss_metric);
  neighboring_cell_info->consec_fail = 0;
  neighboring_cell_info->valid_meas = true;
  neighboring_cell_info->pss_search_start = peak_position - 16;
  neighboring_cell_info->pss_search_length = 32;

  return true;
}

void do_neighboring_cell_measurements(UE_nr_rxtx_proc_t *proc, PHY_VARS_NR_UE *ue, c16_t **rxdata, uint32_t rxdata_size)
{
  NR_DL_FRAME_PARMS *frame_parms = &ue->frame_parms;

  const uint32_t rxdataF_sz = ue->frame_parms.samples_per_slot_wCP;

  // Generate PSS time-domain sequences once for all neighbor cells
  __attribute__((aligned(32))) c16_t pssTime[NUMBER_PSS_SEQUENCE][frame_parms->ofdm_symbol_size];
  for (int nid2_idx = 0; nid2_idx < NUMBER_PSS_SEQUENCE; nid2_idx++) {
    generate_pss_nr_time(frame_parms, nid2_idx, frame_parms->ssb_start_subcarrier, pssTime[nid2_idx]);
  }

  __attribute__((aligned(32))) c16_t rxdataF[ue->frame_parms.nb_antennas_rx][rxdataF_sz];

  for (int cell_idx = 0; cell_idx < NUMBER_OF_NEIGHBORING_CELLS_MAX; cell_idx++) {
    fapi_nr_neighboring_cell_t *neighbor_cell = &ue->nrUE_config.meas_config.nr_neighboring_cell[cell_idx];
    if (neighbor_cell->active == 0) {
      continue;
    }

    memset(rxdataF, 0, sizeof(rxdataF));
    neighboring_cell_info_t *neighboring_cell_info = &ue->measurements.neighboring_cell_info[cell_idx];

    // performing the correlation on a frame length plus two symbols
    // to take into account the possibility of PSS between the two frames
    if (neighboring_cell_info->pss_search_length == 0) {
      neighboring_cell_info->pss_search_length = frame_parms->samples_per_frame + (2 * frame_parms->ofdm_symbol_size);
    }

    bool is_blind_search = (neighbor_cell->Nid_cell == (uint16_t)-1) || (neighbor_cell->Nid_cell == frame_parms->Nid_cell);
    if (neighbor_cell->Nid_cell == frame_parms->Nid_cell) {
      LOG_D(NR_PHY, "Neighbor cell PCI %d matches serving cell, using blind search\n", neighbor_cell->Nid_cell);
      neighboring_cell_info->pss_search_start = 0;
      neighboring_cell_info->pss_search_length = frame_parms->samples_per_frame + (2 * frame_parms->ofdm_symbol_size);
    }
    LOG_D(NR_PHY,
          "Neighbor cell measurement: Nid_cell=%u, is_blind_search=%s, active=%u\n",
          neighbor_cell->Nid_cell,
          is_blind_search ? "true" : "false",
          neighbor_cell->active);

    if (is_blind_search) {
      if (!search_neighboring_cell(frame_parms,
                                   neighbor_cell,
                                   neighboring_cell_info,
                                   rxdata,
                                   rxdata_size,
                                   rxdataF_sz,
                                   rxdataF,
                                   pssTime)) {
        continue;
      }
    } else {
      if (!validate_known_pci(frame_parms, neighbor_cell, neighboring_cell_info, rxdata, rxdataF_sz, rxdataF, pssTime)) {
        if (neighboring_cell_info->consec_fail >= NEIGHBOR_CELL_MAX_CONSECUTIVE_FAILURES) {
          LOG_D(NR_PHY, "Max consecutive failures reached for PCI=%d, resetting to full search\n", neighbor_cell->Nid_cell);
          neighboring_cell_info->pss_search_start = 0;
          neighboring_cell_info->pss_search_length = frame_parms->samples_per_frame + (2 * frame_parms->ofdm_symbol_size);
          neighboring_cell_info->valid_meas = false;
          neighboring_cell_info->consec_fail = 0;
          send_ssb_rsrp_meas(ue, proc, neighbor_cell->Nid_cell, INT_MAX, true, -1, 0.0);
        }
        continue;
      }
    }

    // RSRP measurements
    neighboring_cell_info->ssb_rsrp = nr_ue_calculate_ssb_rsrp(frame_parms, proc, rxdataF, 0, frame_parms->ssb_start_subcarrier);

    neighboring_cell_info->ssb_rsrp_dBm =
        10 * log10(neighboring_cell_info->ssb_rsrp) + 30 - SQ15_SQUARED_NORM_FACTOR_DB
        - ((int)openair0_cfg[ue->rf_map.card].rx_gain[0] - (int)openair0_cfg[ue->rf_map.card].rx_gain_offset[0])
        - dB_fixed(ue->frame_parms.ofdm_symbol_size);

    // Send SS measurements to MAC
    send_ssb_rsrp_meas(ue, proc, neighbor_cell->Nid_cell, neighboring_cell_info->ssb_rsrp_dBm, true, -1, 0.0);
  }
}

void nr_ue_meas_neighboring_cell(void *arg)
{
  nr_meas_task_args_t *args = (nr_meas_task_args_t *)arg;
  do_neighboring_cell_measurements(&args->proc, args->ue, args->rxdata, args->rxdata_size);

  args->ue->measurements.meas_request_pending = false;
  free(args);
}

// This function computes the received noise power
// Measurement units:
// - psd_awgn (AWGN power spectral density):     dBm/Hz
void nr_ue_rrc_measurements(PHY_VARS_NR_UE *ue,
                            const UE_nr_rxtx_proc_t *proc,
                            c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP])
{
  uint8_t k;
  int slot = proc->nr_slot_rx;
  int aarx;
  int16_t *rxF_sss;
  const uint8_t k_left = 48;
  const uint8_t k_right = 183;
  const uint8_t k_length = 8;
  uint8_t l_sss = (ue->symbol_offset + 2) % ue->frame_parms.symbols_per_slot;
  unsigned int ssb_offset = ue->frame_parms.first_carrier_offset + ue->frame_parms.ssb_start_subcarrier;
  double rx_gain = openair0_cfg[ue->rf_map.card].rx_gain[0];
  double rx_gain_offset = openair0_cfg[ue->rf_map.card].rx_gain_offset[0];

  ue->measurements.n0_power_tot = 0;

  LOG_D(PHY, "In %s doing measurements for ssb_offset %d l_sss %d \n", __FUNCTION__, ssb_offset, l_sss);

  for (aarx = 0; aarx<ue->frame_parms.nb_antennas_rx; aarx++) {

    ue->measurements.n0_power[aarx] = 0;
    rxF_sss = (int16_t *)&rxdataF[aarx][l_sss*ue->frame_parms.ofdm_symbol_size];

    //-ve spectrum from SSS
    for(k = k_left; k < k_left + k_length; k++){

      int re = (ssb_offset + k) % ue->frame_parms.ofdm_symbol_size;

      #ifdef DEBUG_MEAS_RRC
      LOG_I(PHY, "In %s -rxF_sss %d %d\n", __FUNCTION__, rxF_sss[re*2], rxF_sss[re*2 + 1]);
      #endif

      ue->measurements.n0_power[aarx] += (((int32_t)rxF_sss[re*2]*rxF_sss[re*2]) + ((int32_t)rxF_sss[re*2 + 1]*rxF_sss[re*2 + 1]));

    }

    //+ve spectrum from SSS
    for(k = k_right; k < k_right + k_length; k++){

      int re = (ssb_offset + k) % ue->frame_parms.ofdm_symbol_size;

      #ifdef DEBUG_MEAS_RRC
      LOG_I(PHY, "In %s +rxF_sss %d %d\n", __FUNCTION__, rxF_sss[re*2], rxF_sss[re*2 + 1]);
      #endif

      ue->measurements.n0_power[aarx] += (((int32_t)rxF_sss[re*2]*rxF_sss[re*2]) + ((int32_t)rxF_sss[re*2 + 1]*rxF_sss[re*2 + 1]));

    }

    ue->measurements.n0_power[aarx] /= 2*k_length;
    ue->measurements.n0_power_dB[aarx] = (unsigned short) dB_fixed(ue->measurements.n0_power[aarx]);
    ue->measurements.n0_power_tot += ue->measurements.n0_power[aarx];

  }

  ue->measurements.n0_power_tot_dB = (unsigned short) dB_fixed(ue->measurements.n0_power_tot);

  #ifdef DEBUG_MEAS_RRC
  const int psd_awgn = -174;
  const int scs = 15000 * (1 << ue->frame_parms.numerology_index);
  const int nf_usrp = ue->measurements.n0_power_tot_dB + 3 + 30 - ((int)rx_gain - (int)rx_gain_offset) - SQ15_SQUARED_NORM_FACTOR_DB - (psd_awgn + dB_fixed(scs) + dB_fixed(ue->frame_parms.ofdm_symbol_size));
  LOG_D(PHY, "In [%s][slot:%d] NF USRP %d dB\n", __FUNCTION__, slot, nf_usrp);
  #endif

  LOG_D(PHY,
        "In [%s][slot:%d] Noise Level %d (digital level %d dB, noise power spectral density %f dBm/RE)\n",
        __FUNCTION__,
        slot,
        ue->measurements.n0_power_tot,
        ue->measurements.n0_power_tot_dB,
        ue->measurements.n0_power_tot_dB + 30 - SQ15_SQUARED_NORM_FACTOR_DB - dB_fixed(ue->frame_parms.ofdm_symbol_size)
            - ((int)rx_gain - (int)rx_gain_offset));
}

// This function implements:
// - PSBCH RSRP calculations according to 38.215 section 5.1.22 Release 16
// - PSBCH DMRS used for calculations
// - TBD: SSS REs for calculation.
// Measurement units:
// - RSRP:    W (dBW)
// returns RXgain to be adjusted based on target rx power (50db) - received digital power in db/RE
int nr_sl_psbch_rsrp_measurements(PHY_VARS_NR_UE *ue,
                                  sl_nr_ue_phy_params_t *sl_phy_params,
                                  NR_DL_FRAME_PARMS *fp,
                                  c16_t rxdataF[][fp->samples_per_slot_wCP],
                                  bool use_SSS)
{
  SL_NR_UE_PSBCH_t *psbch_rx = &sl_phy_params->psbch;
  uint8_t numsym = (fp->Ncp) ? SL_NR_NUM_SYMBOLS_SSB_EXT_CP : SL_NR_NUM_SYMBOLS_SSB_NORMAL_CP;
  uint32_t re_offset = fp->first_carrier_offset + fp->ssb_start_subcarrier;
  uint32_t rsrp = 0, num_re = 0;

  LOG_D(PHY, "PSBCH RSRP MEAS: numsym:%d, re_offset:%d\n", numsym, re_offset);

  for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
    // Calculate PSBCH RSRP based from DMRS REs
    for (uint8_t symbol = 0; symbol < numsym;) {
      struct complex16 *rxF = &rxdataF[aarx][symbol * fp->ofdm_symbol_size];

      for (int re = 0; re < SL_NR_NUM_PSBCH_RE_IN_ONE_SYMBOL; re++) {
        if (re % 4 == 0) { // DMRS RE
          uint16_t offset = (re_offset + re) % fp->ofdm_symbol_size;

          rsrp += c16amp2(rxF[offset]);
          num_re++;
        }
      }
      symbol = (symbol == 0) ? 5 : symbol + 1;
    }
  }

  if (use_SSS) {
    // TBD...
    // UE can decide between using only PSBCH DMRS or PSBCH DMRS and SSS for PSBCH RSRP computation.
    // If needed this can be implemented. Reference Spec 38.215
  }

  psbch_rx->rsrp_dB_per_RE = 10 * log10(rsrp / num_re);
  psbch_rx->rsrp_dBm_per_RE =
      psbch_rx->rsrp_dB_per_RE + 30 - SQ15_SQUARED_NORM_FACTOR_DB
      - ((int)openair0_cfg[ue->rf_map.card].rx_gain[0] - (int)openair0_cfg[ue->rf_map.card].rx_gain_offset[0])
      - dB_fixed(fp->ofdm_symbol_size);

  int adjust_rxgain = TARGET_RX_POWER - psbch_rx->rsrp_dB_per_RE;

  LOG_D(PHY,
        "PSBCH RSRP (DMRS REs): numREs:%d RSRP :%d dB/RE ,RSRP:%d dBm/RE, adjust_rxgain:%d dB\n",
        num_re,
        psbch_rx->rsrp_dB_per_RE,
        psbch_rx->rsrp_dBm_per_RE,
        adjust_rxgain);

  return adjust_rxgain;
}
