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

/*! \file nr_transport_ue.h
 * \brief data structures for PDSCH/DLSCH/PUSCH/ULSCH physical and transport channel descriptors (TX/RX)
 * \author R. Knopp
 * \date 2011
 * \version 0.1
 * \company Eurecom
 * \email: raymond.knopp@eurecom.fr, florian.kaltenberger@eurecom.fr, oscar.tonelli@yahoo.it
 * \note
 * \warning
 */
#ifndef __NR_TRANSPORT_UE__H__
#define __NR_TRANSPORT_UE__H__
#include <limits.h>
#include "PHY/impl_defs_top.h"

#include "PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
#include "../NR_TRANSPORT/nr_transport_common_proto.h"

#define MAX_FA_BLOCKS 10
typedef struct {
  int start[MAX_FA_BLOCKS];
  int end[MAX_FA_BLOCKS];
  int num_rbs;
  int num_blocks;
  uint8_t bitmap[36];
} freq_alloc_bitmap_t;

typedef struct {
  /// Index of current HARQ round for this ULSCH
  uint8_t round;
  /// pointer to pdu from MAC interface (TS 36.212 V15.4.0, Sec 5.1 p. 8)
  unsigned char *payload_AB;
  /// Pointers to transport block segments
  uint8_t **c;
  /// LDPC-code outputs
  uint8_t **d;
  /// LDPC-code outputs (TS 36.212 V15.4.0, Sec 5.3.2 p. 17)
  uint8_t *e;
  /// Rate matching (Interleaving) outputs (TS 36.212 V15.4.0, Sec 5.4.2.2 p. 30)
  uint8_t *f;
  /// Number of code segments
  uint32_t C;
  /// Number of bits in code segments
  uint32_t K;
  ///
  uint32_t Kb;
  /// Number of "Filler" bits
  uint32_t F;
  /// Number of soft channel bits
  uint32_t G;
  // Number of modulated symbols carrying data
  uint32_t num_of_mod_symbols;
  // Encoder BG
  uint8_t BG;
  // LDPC lifting size
  uint32_t Z;
} NR_UL_UE_HARQ_t;

typedef struct {
  SCH_status_t status;
  /// NDAPI struct for UE
  nfapi_nr_ue_pusch_pdu_t pusch_pdu;
  // UL number of harq processes
  uint8_t number_harq_processes_for_pusch;
  /// RNTI type
  nr_rnti_type_t rnti_type;
  /// Cell ID
  int     Nid_cell;
  /// bit mask of PT-RS ofdm symbol indicies
  uint16_t ptrs_symbols;
} NR_UE_ULSCH_t;

typedef struct {
  /// Indicator of first reception
  uint8_t first_rx;
  /// DLSCH status flag indicating
  SCH_status_t status;
  /// Pointer to the payload (38.212 V15.4.0 section 5.1)
  uint8_t *b;
  /// Pointers to transport block segments
  uint8_t **c;
  /// soft bits for each received segment ("d"-sequence)(for definition see 36-212 V8.6 2009-03, p.15)
  /// Accumulates the soft bits for each round to increase decoding success (HARQ)
  int16_t **d;
  /// Index of current HARQ round for this DLSCH
  uint8_t DLround;
  /// Number of code segments 
  uint32_t C;
  /// Number of bits in code segments
  uint32_t K;
  /// Number of "Filler" bits 
  uint32_t F;
  /// LDPC lifting factor
  uint32_t Z;
  /// codeword this transport block is mapped to
  uint8_t codeword;
  /// HARQ-ACKs
  bool decodeResult;
  /// Last index of LLR buffer that contains information.
  /// Used for computing LDPC decoder R
  int llrLen;
  /// Number of segments processed so far
  uint32_t processedSegments;
  decode_abort_t abort_decode;
} NR_DL_UE_HARQ_t;

typedef struct {
  /// RNTI
  uint16_t rnti;
  /// RNTI type
  uint8_t rnti_type;
  /// Active flag for DLSCH demodulation
  bool active;
  /// Structure to hold dlsch config from MAC
  fapi_nr_dl_config_dlsch_pdu_rel15_t dlsch_config;
  /// Number of MIMO layers (streams) 
  uint8_t Nl;
  /// Maximum number of LDPC iterations
  uint8_t max_ldpc_iterations;
  /// number of iterations used in last turbo decoding
  int8_t last_iteration_cnt;
  /// bit mask of PT-RS ofdm symbol indicies
  uint16_t ptrs_symbols;
  // PTRS symbol index, to be updated every PTRS symbol within a slot.
  uint8_t ptrs_symbol_index;
} NR_UE_DLSCH_t;

typedef struct {
  uint16_t Q_dash_ACK; // number of coded HARQ-ACK symbols
  uint16_t E_uci_ACK; // number of coded HARQ-ACK bits
  uint16_t Q_dash_ACK_rvd; // number of coded HARQ-ACK symbols reserved
  uint16_t E_uci_ACK_rvd; // number of coded HARQ-ACK bits reserved
  uint16_t Q_dash_CSI1; // number of coded CSI part 1 symbols
  uint16_t E_uci_CSI1; // number of coded CSI part 1 bits
  uint16_t Q_dash_CSI2; // number of coded CSI part 2 symbols
  uint16_t E_uci_CSI2; // number of coded CSI part 2 bits
  uint32_t G_ulsch; // bit capacity of ULSCH
} rate_match_info_uci_t;

#endif
