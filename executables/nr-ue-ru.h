/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.0  (the "License"); you may not use this file
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

#ifndef NR_UE_RU_H
#define NR_UE_RU_H

#include "PHY/defs_nr_UE.h"
#include "radio/COMMON/common_lib.h"

int nrue_get_cell_count(void);
const nrUE_cell_params_t *nrue_get_cell(int cell_id);
NR_DL_FRAME_PARMS *nrue_get_cell_fp(int cell_id);
void nrue_set_cell(int cell_id, const nrUE_cell_params_t *cell);
void nrue_set_cell_params(configmodule_interface_t *cfg);

int nrue_get_ru_count(void);
const nrUE_RU_params_t *nrue_get_ru(int ru_id);
void nrue_set_ru_cell_id(int ru_id, int cell_id);
void nrue_set_ru_params(configmodule_interface_t *cfg);

void nrue_init_openair0(void);

void nrue_ru_start(void);
void nrue_ru_end(void);
void nrue_ru_set_freq(PHY_VARS_NR_UE *UE, uint64_t ul_carrier, uint64_t dl_carrier, int freq_offset);
int nrue_ru_adjust_rx_gain(PHY_VARS_NR_UE *UE, int gain_change);
int nrue_ru_read(PHY_VARS_NR_UE *UE, openair0_timestamp_t *ptimestamp, void **buff, int nsamps, int num_antennas);
int nrue_ru_write(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, void **buff, int nsamps, int num_antennas, int flags);
int nrue_ru_write_reorder(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, void **txp, int nsamps, int nbAnt, int flags);
void nrue_ru_write_reorder_clear_context(PHY_VARS_NR_UE *UE);

#endif
