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

/*! \file PHY/NR_TRANSPORT/nr_ulsch.c
* \brief Top-level routines for the reception of the PUSCH TS 38.211 v 15.4.0
* \author Ahmed Hussein
* \date 2019
* \version 0.1
* \company Fraunhofer IIS
* \email: ahmed.hussein@iis.fraunhofer.de
* \note
* \warning
*/

#include <stdint.h>
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_ulsch.h"
#include "SCHED_NR/sched_nr.h"

static NR_gNB_ULSCH_t *find_nr_ulsch(PHY_VARS_gNB *gNB, uint16_t rnti, int pid)
{
  int16_t first_free_index = -1;
  AssertFatal(gNB != NULL, "gNB is null\n");
  NR_gNB_ULSCH_t *ulsch = NULL;

  for (int i = 0; i < gNB->max_nb_pusch; i++) {
    ulsch = &gNB->ulsch[i];
    AssertFatal(ulsch, "gNB->ulsch[%d] is null\n", i);
    if (!ulsch->active) {
      if (first_free_index == -1)
        first_free_index = i;
    } else {
      // if there is already an active ULSCH for this RNTI and HARQ_PID
      if ((ulsch->harq_pid == pid) && (ulsch->rnti == rnti))
        return ulsch;
    }
  }
  if (first_free_index != -1)
    ulsch = &gNB->ulsch[first_free_index];

  return ulsch;
}

static void dump_pusch_pdu(int instance, int frame, int slot, nfapi_nr_pusch_pdu_t *pusch_pdu)
{
  LOG_D(PHY,
        "[gNB %d] %d.%d ULSCH PDU pdu_bit_map %u "
        "rnti %u "
        "handle %u "
        "bwp_size %u "
        "bwp_start %u "
        "subcarrier_spacing %u "
        "cyclic_prefix %u "
        "target_code_rate %u "
        "qam_mod_order %u "
        "mcs_index %u "
        "mcs_table %u "
        "transform_precoding %u "
        "data_scrambling_id %u "
        "nrOfLayers %u "
        "ul_dmrs_symb_pos %u "
        "dmrs_config_type %u "
        "ul_dmrs_scrambling_id %u "
        "scid %u "
        "num_dmrs_cdm_grps_no_data %u "
        "dmrs_ports %u "
        "resource_alloc %u "
        "rb_start %u "
        "rb_size %u "
        "vrb_to_prb_mapping %u "
        "frequency_hopping %u "
        "tx_direct_current_location %u "
        "uplink_frequency_shift_7p5khz %u "
        "start_symbol_index %u "
        "nr_of_symbols %u "
        "tbslbrm %u "
        "ldpcBaseGraph %u "
        "pusch_data->rv_index %u "
        "pusch_data->harq_process_id %u "
        "pusch_data->new_data_indicator %u "
        "pusch_data->num_cb %u\n",
        instance,
        frame,
        slot,
        pusch_pdu->pdu_bit_map,
        pusch_pdu->rnti,
        pusch_pdu->handle,
        pusch_pdu->bwp_size,
        pusch_pdu->bwp_start,
        pusch_pdu->subcarrier_spacing,
        pusch_pdu->cyclic_prefix,
        pusch_pdu->target_code_rate,
        pusch_pdu->qam_mod_order,
        pusch_pdu->mcs_index,
        pusch_pdu->mcs_table,
        pusch_pdu->transform_precoding,
        pusch_pdu->data_scrambling_id,
        pusch_pdu->nrOfLayers,
        pusch_pdu->ul_dmrs_symb_pos,
        pusch_pdu->dmrs_config_type,
        pusch_pdu->ul_dmrs_scrambling_id,
        pusch_pdu->scid,
        pusch_pdu->num_dmrs_cdm_grps_no_data,
        pusch_pdu->dmrs_ports,
        pusch_pdu->resource_alloc,
        pusch_pdu->rb_start,
        pusch_pdu->rb_size,
        pusch_pdu->vrb_to_prb_mapping,
        pusch_pdu->frequency_hopping,
        pusch_pdu->tx_direct_current_location,
        pusch_pdu->uplink_frequency_shift_7p5khz,
        pusch_pdu->start_symbol_index,
        pusch_pdu->nr_of_symbols,
        pusch_pdu->maintenance_parms_v3.tbSizeLbrmBytes,
        pusch_pdu->maintenance_parms_v3.ldpcBaseGraph,
        pusch_pdu->pusch_data.rv_index,
        pusch_pdu->pusch_data.harq_process_id,
        pusch_pdu->pusch_data.new_data_indicator,
        pusch_pdu->pusch_data.num_cb);
}


void nr_fill_ulsch(PHY_VARS_gNB *gNB, int frame, int slot, nfapi_nr_pusch_pdu_t *ulsch_pdu)
{
  dump_pusch_pdu(gNB->Mod_id, frame, slot, ulsch_pdu);
  int harq_pid = ulsch_pdu->pusch_data.harq_process_id;
  NR_gNB_ULSCH_t *ulsch = find_nr_ulsch(gNB, ulsch_pdu->rnti, harq_pid);
  if (ulsch == NULL) {
    LOG_E(NR_PHY, "No ulsch_id found for rnti %04x\n", ulsch_pdu->rnti);
    return;
  }

  ulsch->rnti = ulsch_pdu->rnti;
  ulsch->harq_pid = harq_pid;
  ulsch->handled = 0;
  ulsch->active = true;
  ulsch->beam_nb = 0;
  if (gNB->common_vars.beam_id) {
    int fapi_beam_idx = ulsch_pdu->beamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx;
    int bitmap = SL_to_bitmap(ulsch_pdu->start_symbol_index, ulsch_pdu->nr_of_symbols);
    ulsch->beam_nb = beam_index_allocation(gNB->enable_analog_das,
                                           fapi_beam_idx,
                                           &gNB->common_vars,
                                           slot,
                                           NR_NUMBER_OF_SYMBOLS_PER_SLOT,
                                           bitmap);
  }
  ulsch->frame = frame;
  ulsch->slot = slot;

  NR_UL_gNB_HARQ_t *harq = ulsch->harq_process;
  if (ulsch_pdu->pusch_data.new_data_indicator)
    harq->harq_to_be_cleared = true;
  LOG_D(NR_PHY,
        "NEW ULSCH %d.%d RNTI %x HARQ PID %d new data indicator %d\n",
        frame,
        slot,
        ulsch_pdu->rnti,
        harq_pid,
        ulsch_pdu->pusch_data.new_data_indicator);

  if (ulsch_pdu->pusch_data.new_data_indicator)
    harq->round = 0;
  else
    harq->round++;

  memcpy(&ulsch->harq_process->ulsch_pdu, ulsch_pdu, sizeof(ulsch->harq_process->ulsch_pdu));
  LOG_D(PHY, "Initializing nFAPI for ULSCH, harq_pid %d, layers %d\n", harq_pid, ulsch_pdu->nrOfLayers);
}

void reset_active_ulsch(PHY_VARS_gNB *gNB, int frame)
{
  // deactivate ULSCH structure after a given number of frames
  // no activity on this structure for NUMBER_FRAMES_PHY_UE_INACTIVE
  // assuming UE disconnected or some other error occurred
  for (int i = 0; i < gNB->max_nb_pusch; i++) {
    NR_gNB_ULSCH_t *ulsch = &gNB->ulsch[i];
    int diff = (frame - ulsch->frame + 1024) & 1023;
    if (ulsch->active && diff > NUMBER_FRAMES_PHY_UE_INACTIVE && diff < 100) {
      ulsch->active = false;
      LOG_D(NR_PHY,
            "Frame %d: resetting ulsch %d harq %d (programmed in %d.%d)\n",
            frame,
            i,
            ulsch->harq_pid,
            ulsch->frame,
            ulsch->slot);
    }
  }
}

void nr_ulsch_unscrambling(int16_t* llr, uint32_t size, uint32_t Nid, uint32_t n_RNTI)
{
  nr_codeword_unscrambling(llr, size, 0, Nid, n_RNTI);
}

void nr_ulsch_layer_demapping(int16_t *llr_cw, uint8_t Nl, uint8_t mod_order, uint32_t length, int16_t **llr_layers)
{

  switch (Nl) {
    case 1:
      memcpy((void*)llr_cw, (void*)llr_layers[0], (length)*sizeof(int16_t));
      break;
    case 2:
    case 3:
    case 4:
      for (int i=0; i<(length/Nl/mod_order); i++) {
        for (int l=0; l<Nl; l++) {
          for (int m=0; m<mod_order; m++) {
            llr_cw[i*Nl*mod_order+l*mod_order+m] = llr_layers[l][i*mod_order+m];
          }
        }
      }
      break;
  default:
    AssertFatal(0, "Not supported number of layers %d\n", Nl);
  }
}

void dump_pusch_stats(FILE *fd, PHY_VARS_gNB *gNB)
{
  for (int i = 0; i < MAX_MOBILES_PER_GNB; i++) {
    NR_gNB_PHY_STATS_t *stats = &gNB->phy_stats[i];
    if (stats->active && stats->frame != stats->ulsch_stats.dump_frame) {
      stats->ulsch_stats.dump_frame = stats->frame;
      for (int aa = 0; aa < gNB->frame_parms.nb_antennas_rx; aa++)
        if (aa == 0)
          fprintf(fd,
                  "ULSCH RNTI %4x, %d: ulsch_power[%d] %d,%d ulsch_noise_power[%d] %d.%d, sync_pos %d\n",
                  stats->rnti,
                  stats->frame,
                  aa,
                  stats->ulsch_stats.power[aa] / 10,
                  stats->ulsch_stats.power[aa] % 10,
                  aa,
                  stats->ulsch_stats.noise_power[aa] / 10,
                  stats->ulsch_stats.noise_power[aa] % 10,
                  stats->ulsch_stats.sync_pos);
        else
          fprintf(fd,
                  "                  ulsch_power[%d] %d.%d, ulsch_noise_power[%d] %d.%d\n",
                  aa,
                  stats->ulsch_stats.power[aa] / 10,
                  stats->ulsch_stats.power[aa] % 10,
                  aa,
                  stats->ulsch_stats.noise_power[aa] / 10,
                  stats->ulsch_stats.noise_power[aa] % 10);

      int *rt = stats->ulsch_stats.round_trials;
      fprintf(fd,
              "                 round_trials %d(%1.1e):%d(%1.1e):%d(%1.1e):%d, DTX %d, current_Qm %d, current_RI %d, total_bytes "
              "RX/SCHED %d/%d\n",
              rt[0],
              (double)rt[1] / rt[0],
              rt[1],
              (double)rt[2] / rt[0],
              rt[2],
              (double)rt[3] / rt[0],
              rt[3],
              stats->ulsch_stats.DTX,
              stats->ulsch_stats.current_Qm,
              stats->ulsch_stats.current_RI,
              stats->ulsch_stats.total_bytes_rx,
              stats->ulsch_stats.total_bytes_tx);
    }
  }
}
