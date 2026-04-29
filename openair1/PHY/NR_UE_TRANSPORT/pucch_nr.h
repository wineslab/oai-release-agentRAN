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

/*! \file PHY/NR_UE_TRANSPORT/pucch_nr.c
* \brief Top-level routines for generating the PUCCH physical channel
* \author A. Mico Pereperez
* \date 2018
* \version 0.1
* \company Eurecom
* \email:
* \note
* \warning
*/
#ifndef __PUCCH_NR__H__
#define __PUCCH_NR__H__

//#include "PHY/defs.h"
#include "PHY/impl_defs_nr.h"
#include "PHY/defs_nr_common.h"
#include "PHY/defs_nr_UE.h"
//#include "PHY/extern.h"

#include "common/utils/LOG/log.h"
#include "common/utils/LOG/vcd_signal_dumper.h"

#include "T.h"
#define ONE_OVER_SQRT2 23170 // 32767/sqrt(2) = 23170 (ONE_OVER_SQRT2)

void nr_generate_pucch0(const PHY_VARS_NR_UE *ue,
                        c16_t **txdataF,
                        const NR_DL_FRAME_PARMS *frame_parms,
                        const int16_t amp,
                        const int nr_slot_tx,
                        const fapi_nr_ul_config_pucch_pdu *pucch_pdu);

void nr_generate_pucch1(const PHY_VARS_NR_UE *ue,
                        c16_t **txdataF,
                        const NR_DL_FRAME_PARMS *frame_parms,
                        const int16_t amp,
                        const int nr_slot_tx,
                        const fapi_nr_ul_config_pucch_pdu *pucch_pdu);

void nr_generate_pucch2(const PHY_VARS_NR_UE *ue,
                        c16_t **txdataF,
                        const NR_DL_FRAME_PARMS *frame_parms,
                        const int16_t amp,
                        const int nr_slot_tx,
                        const fapi_nr_ul_config_pucch_pdu *pucch_pdu);

void nr_generate_pucch3_4(const PHY_VARS_NR_UE *ue,
                          c16_t **txdataF,
                          const NR_DL_FRAME_PARMS *frame_parms,
                          const int16_t amp,
                          const int nr_slot_tx,
                          const fapi_nr_ul_config_pucch_pdu *pucch_pdu);

void nr_uci_encoding(uint64_t payload, uint8_t nr_bit, uint8_t nrofPRB, bool uci_on_pusch, uint16_t E, uint8_t Qm, uint64_t *b);

static const uint8_t list_of_prime_numbers[46] = {2,  3,  5,  7,  11, 13, 17, 19, 23, 29,
                                         31, 37, 41, 43, 47, 53, 59, 61, 67, 71,
                                         73, 79, 83, 89, 97, 101,103,107,109,113,
                                         127,131,137,139,149,151,157,163,167,173,
                                         179,181,191,193,197,199};
#endif
