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

#ifndef _ORAN_ISOLATE_H_
#define _ORAN_ISOLATE_H_

#include <stdio.h>

#include <pthread.h>
#include <stdint.h>

#include "xran_fh_o_du.h"
#include "openair1/PHY/impl_defs_nr.h"
#include "openair1/PHY/TOOLS/tools_defs.h"
/*
 * Structure added to bear the information needed from OAI RU
 */
typedef struct ru_info_s {
  // Needed for UL
  int nb_rx;
  int32_t **rxdataF;

  // Needed for DL
  int nb_tx;
  int32_t **txdataF_BF;

  /// \brief Anaglogue beam ID for each OFDM symbol (used when beamforming not done in RU)
  /// - first index: concurrent beam
  /// - second index: beam_id [0.. symbols_per_frame]
  int32_t **beam_id;

  /// number of concurrent analog beams in period
  int num_beams_period;

  // Needed for Prach
  c16_t (*prach_buf)[NB_ANTENNAS_RX][NR_PRACH_SEQ_LEN_L];
} ru_info_t;

/** @brief Reads RX data (PRACH/PUSCH) of next slot.
 *
 * @param ru pointer to structure keeping pointers to OAI data.
 * @param frame output of the frame which has been read.
 * @param slot output of the slot which has been read. */
int xran_fh_rx_read_slot(ru_info_t *ru, int *frame, int *slot);
#ifdef F_RELEASE
int xran_fh_rx_read_slot_BySymbol(ru_info_t *ru, int *frame, int *slot);
#endif
/** @brief Writes TX data (PDSCH) of given slot. */
int xran_fh_tx_send_slot(ru_info_t *ru, int frame, int slot, uint64_t timestamp);
#ifdef F_RELEASE
int xran_fh_tx_send_slot_BySymbol(ru_info_t *ru, int frame, int slot, uint64_t timestamp);
#endif

#endif /* _ORAN_ISOLATE_H_ */
