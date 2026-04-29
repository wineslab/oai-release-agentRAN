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

#ifndef _RRC_GNB_ASN1_H_
#define _RRC_GNB_ASN1_H_

#include <stdbool.h>
#include "seq_arr.h"
#include "NR_DRB-ToAddMod.h"
#include "NR_RadioBearerConfig.h"
#include "openair2/LAYER2/nr_pdcp/nr_pdcp_configuration.h"

NR_PDCP_Config_t *nr_rrc_build_pdcp_config_ie(const bool integrity, const bool ciphering, const nr_pdcp_configuration_t *pdcp);

#endif
