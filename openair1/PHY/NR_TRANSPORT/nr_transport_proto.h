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

/*! \file nr_transport_proto.h
 * \brief Function prototypes for PHY physical/transport channel processing and generation
 * \author Ahmed Hussein
 * \date 2019
 * \version 0.1
 * \company Fraunhofer IIS
 * \email: ahmed.hussein@iis.fraunhofer.de
 * \note
 * \warning
 */

#ifndef __NR_TRANSPORT__H__
#define __NR_TRANSPORT__H__

#include "PHY/defs_nr_common.h"
#include "PHY/defs_gNB.h"

#define NR_PBCH_PDU_BITS 24

NR_gNB_PHY_STATS_t *get_phy_stats(PHY_VARS_gNB *gNB, uint16_t rnti);

int nr_generate_prs(int slot,
                    c16_t *txdataF,
                    int16_t amp,
                    prs_config_t *prs_cfg,
                    nfapi_nr_config_request_scf_t *config,
                    const NR_DL_FRAME_PARMS *frame_parms);

/*!
\fn int nr_generate_pss
\brief Generation of the NR PSS
@param
@returns 0 on success
 */
int nr_generate_pss(c16_t *txdataF,
                    int16_t amp,
                    uint8_t ssb_start_symbol,
                    nfapi_nr_config_request_scf_t *config,
                    NR_DL_FRAME_PARMS *frame_parms);

/*!
\fn int nr_generate_sss
\brief Generation of the NR SSS
@param
@returns 0 on success
 */
int nr_generate_sss(c16_t *txdataF,
                    int16_t amp,
                    uint8_t ssb_start_symbol,
                    nfapi_nr_config_request_scf_t *config,
                    NR_DL_FRAME_PARMS *frame_parms);

/*!
\fn void nr_generate_pbch_dmrs
\brief Generation of the DMRS for the PBCH
@param
 */
void nr_generate_pbch_dmrs(uint32_t *gold_pbch_dmrs,
                           c16_t *txdataF,
                           int16_t amp,
                           uint8_t ssb_start_symbol,
                           nfapi_nr_config_request_scf_t *config,
                           NR_DL_FRAME_PARMS *frame_parms);

/*!
\fn void nr_generate_pbch
\brief Generation of the PBCH
@param
 */
void nr_generate_pbch(PHY_VARS_gNB *gNB,
                      const nfapi_nr_dl_tti_ssb_pdu *ssb_pdu,
                      c16_t *txdataF,
                      uint8_t ssb_start_symbol,
                      uint8_t n_hf,
                      int sfn,
                      nfapi_nr_config_request_scf_t *config,
                      NR_DL_FRAME_PARMS *frame_parms);

/*!
\fn int nr_generate_pbch
\brief PBCH interleaving function
@param bit index i of the input payload
@returns the bit index of the output
 */
void nr_init_pbch_interleaver(uint8_t *interleaver);
uint32_t nr_pbch_extra_byte_generation(int sfn, int n_hf, int ssb_index, int ssb_sc_offset, int Lmax);

NR_gNB_DLSCH_t new_gNB_dlsch(NR_DL_FRAME_PARMS *frame_parms, uint16_t N_RB);

void free_gNB_dlsch(NR_gNB_DLSCH_t *dlsch, uint16_t N_RB, const NR_DL_FRAME_PARMS *frame_parms);

/** \brief This function is the top-level entry point to PUSCH demodulation, after frequency-domain transformation and channel estimation.  It performs
    - RB extraction (signal and channel estimates)
    - channel compensation (matched filtering)
    - RE extraction (dmrs)
    - antenna combining (MRC, Alamouti, cycling)
    - LLR computation
    This function supports TM1, 2, 3, 5, and 6.
    @param ue Pointer to PHY variables
    @param UE_id id of current UE
    @param frame Frame number
    @param slot Slot number
    @param harq_pid HARQ process ID
*/
int nr_rx_pusch_tp(PHY_VARS_gNB *gNB,
                   uint8_t ulsch_id,
                   uint32_t frame,
                   uint8_t slot,
                   unsigned char harq_pid,
                   int beam_nb);

/*!
\brief This function implements the idft transform precoding in PUSCH
\param z Pointer to input in frequnecy domain, and it is also the output in time domain
\param Msc_PUSCH number of allocated data subcarriers
*/
void nr_idft(int32_t *z, uint32_t Msc_PUSCH);

void nr_ulsch_qpsk_qpsk(c16_t *stream0_in, 
                        c16_t *stream1_in, 
                        c16_t *stream0_out, 
                        c16_t *rho01, 
                        uint32_t length);

void nr_ulsch_qam16_qam16(c16_t *stream0_in,
                          c16_t *stream1_in,
                          c16_t *ch_mag,
                          c16_t *ch_mag_i,
                          c16_t *stream0_out,
                          c16_t *rho01,
                          uint32_t length);

void nr_ulsch_qam64_qam64(c16_t *stream0_in,
                          c16_t *stream1_in,
                          c16_t *ch_mag,
                          c16_t *ch_mag_i,
                          c16_t *stream0_out,
                          c16_t *rho01,
                          uint32_t length);

/** \brief This function computes the log-likelihood ratios for 4, 16, and 64 QAM
    @param rxdataF_comp Compensated channel output
    @param ul_ch_mag  uplink channel magnitude multiplied by the 1st amplitude threshold in QAM 64
    @param ul_ch_magb uplink channel magnitude multiplied by the 2bd amplitude threshold in QAM 64
    @param ulsch_llr llr output
    @param nb_re number of REs for this allocation
    @param symbol OFDM symbol index in sub-frame
    @param mod_order modulation order
*/
void nr_ulsch_compute_llr(int32_t *rxdataF_comp,
                          c16_t *ul_ch_mag,
                          c16_t *ul_ch_magb,
                          c16_t *ul_ch_magc,
                          int16_t *ulsch_llr,
                          uint32_t nb_re,
                          uint8_t symbol,
                          uint8_t mod_order);

void reset_active_stats(PHY_VARS_gNB *gNB, int frame);
void reset_active_ulsch(PHY_VARS_gNB *gNB, int frame);

void nr_ulsch_compute_ML_llr(NR_gNB_PUSCH *pusch_vars,
                             uint32_t symbol,
                             c16_t *rxdataF_comp0,
                             c16_t *rxdataF_comp1,
                             c16_t *ul_ch_mag0,
                             c16_t *ul_ch_mag1,
                             int16_t *llr_layers0,
                             int16_t *llr_layers1,
                             c16_t *rho0,
                             c16_t *rho1,
                             uint32_t nb_re,
                             uint8_t mod_order);

void nr_ulsch_shift_llr(int16_t **llr_layers, uint32_t nb_re, uint32_t rxdataF_ext_offset, uint8_t mod_order, int shift);

void nr_fill_ulsch(PHY_VARS_gNB *gNB,
                   int frame,
                   int slot,
                   nfapi_nr_pusch_pdu_t *ulsch_pdu);

prach_item_t *nr_schedule_rx_prach(PHY_VARS_gNB *gNB, int SFN, int Slot, nfapi_nr_prach_pdu_t *prach_pdu);

typedef struct rx_prach_out {
  uint16_t max_preamble;
  uint16_t max_preamble_energy;
  uint16_t max_preamble_delay;
} rx_prach_out_t;
rx_prach_out_t rx_nr_prach(const prach_item_t *, int occasion);

void rx_nr_prach_ru(prach_item_t *, int32_t **, NR_DL_FRAME_PARMS *frame_parms, int N_TA_offset);

prach_item_t *find_nr_prach(prach_list_t *, int frame, int slot, find_type_t type);
void nr_fill_pucch(PHY_VARS_gNB *gNB,
                   int frame,
                   int slot,
                   nfapi_nr_pucch_pdu_t *pucch_pdu);

void nr_fill_srs(PHY_VARS_gNB *gNB,
                 frame_t frame,
                 slot_t slot,
                 nfapi_nr_srs_pdu_t *srs_pdu);

int nr_get_srs_signal(PHY_VARS_gNB *gNB,
                      c16_t **rxdataF,
                      frame_t frame,
                      slot_t slot,
                      nfapi_nr_srs_pdu_t *srs_pdu,
                      nr_srs_info_t *nr_srs_info,
                      c16_t srs_received_signal[][gNB->frame_parms.ofdm_symbol_size * (1 << srs_pdu->num_symbols)],
                      c16_t srs_received_noise[][gNB->frame_parms.ofdm_symbol_size * (1 << srs_pdu->num_symbols)]);

void nr_srs_rx_procedures(PHY_VARS_gNB *gNB,
                          int frame_rx,
                          int slot_rx,
                          uint8_t nb_antennas_rx,
                          uint8_t N_ap,
                          uint8_t N_symb_SRS,
                          uint16_t ofdm_symbol_size,
                          NR_gNB_SRS_t *srs,
                          nr_srs_info_t *nr_srs_info,
                          int *srs_est,
                          c16_t srs_estimated_channel_freq[][N_ap][ofdm_symbol_size * N_symb_SRS],
                          c16_t srs_estimated_channel_time[][N_ap][NR_SRS_IDFT_OVERSAMP_FACTOR * ofdm_symbol_size],
                          int16_t *snr_per_rb,
                          uint16_t *timing_advance_offset,
                          int16_t *timing_advance_offset_nsec);

int get_nr_prach_duration(uint8_t prach_format);

void free_nr_prach_entry(prach_list_t *, prach_item_t *);

void nr_decode_pucch1(c16_t **rxdataF,
                      pucch_GroupHopping_t pucch_GroupHopping,
                      uint32_t n_id,       // hoppingID higher layer parameter
                      uint64_t *payload,
                      NR_DL_FRAME_PARMS *frame_parms,
                      int16_t amp,
                      int nr_tti_tx,
                      uint8_t m0,
                      uint8_t nrofSymbols,
                      uint8_t startingSymbolIndex,
                      uint16_t startingPRB,
                      uint16_t startingPRB_intraSlotHopping,
                      uint8_t timeDomainOCC,
                      uint8_t nr_bit);

void nr_decode_pucch2(PHY_VARS_gNB *gNB,
                      c16_t **rxdataF,
                      int frame,
                      int slot,
                      nfapi_nr_uci_pucch_pdu_format_2_3_4_t* uci_pdu,
                      nfapi_nr_pucch_pdu_t* pucch_pdu);

void nr_decode_pucch0(PHY_VARS_gNB *gNB,
                      c16_t **rxdataF,
                      int frame,
                      int slot,
                      nfapi_nr_uci_pucch_pdu_format_0_1_t* uci_pdu,
                      nfapi_nr_pucch_pdu_t* pucch_pdu);


#endif /*__NR_TRANSPORT__H__*/
