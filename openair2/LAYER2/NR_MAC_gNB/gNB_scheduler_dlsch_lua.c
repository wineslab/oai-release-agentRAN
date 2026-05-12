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

/*
 * Lua-based DL scheduling policy.
 *
 * This file is a self-contained scheduling policy that plugs into the
 * nr_dl_sched_policy_fn function pointer. It converts the C candidate
 * structs into an FFI-compatible dl_ue_metric_t array, calls the Lua
 * compute_dl_allocations() function, and writes the results back into
 * nr_dl_alloc_t.
 *
 * Enable by setting LUA_SCHED=<path_to_script.lua>
 */

#include "common/utils/nr/nr_common.h"
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include <luajit-2.1/lua.h>
#include <luajit-2.1/lualib.h>
#include <luajit-2.1/lauxlib.h>
#include <pthread.h>

/*
 * The dl_ue_metric_t struct is the FFI-compatible data structure shared between
 * C and the Lua scheduling script. It must match the FFI cdef in the Lua
 * script exactly (field order, types, and packing).
 */
typedef struct {
  uint16_t rnti;
  uint8_t  nr_of_layers;
  uint32_t pending_bytes;
  float    throughput;
  uint8_t  previous_mcs;
  int      ue_type;           /* 0=new_data, 1=retx */
  uint16_t required_rbs;      /* retx: rbSize from HARQ, new: 0 */
  uint8_t  required_mcs;      /* retx: saved MCS, new: 255 */
  uint16_t cqi;
  int      uid;
  int      mcs_table;
  float    bler;
  int      slot;
  int      frame;
  uint64_t fiveQI;
  int16_t  channel_mag_per_rb[272]; /* future: SRS */
  uint32_t bwp_start;
  uint32_t bwp_size;
  int16_t  dl_rsrp;
  uint64_t hol_delay_us;    /* head-of-line delay in microseconds */
  uint16_t allocated_rb;       /* output: filled by Lua */
  uint8_t  allocated_mcs;      /* output: filled by Lua */
  uint16_t allocated_rb_start; /* output: filled by Lua */
} dl_ue_metric_t;

static lua_State *lua_dl_state = NULL;
static pthread_mutex_t lua_dl_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_lua_dl_scheduler(void)
{
  const char *script_path = getenv("LUA_SCHED");
  if (!script_path || script_path[0] == '\0') {
    LOG_W(NR_MAC, "LUA_SCHED env var not set — DL Lua scheduler disabled, using C proportional fair\n");
    return;
  }

  lua_dl_state = luaL_newstate();
  AssertFatal(lua_dl_state != NULL, "Failed to create Lua DL state\n");
  luaL_openlibs(lua_dl_state);

  int err = luaL_dofile(lua_dl_state, script_path);
  if (err) {
    LOG_E(NR_MAC, "Lua DL scheduler: failed to load '%s': %s\n",
          script_path, lua_tostring(lua_dl_state, -1));
    lua_close(lua_dl_state);
    lua_dl_state = NULL;
    AssertFatal(false, "Lua DL scheduler initialization failed\n");
  }

  lua_getglobal(lua_dl_state, "compute_dl_allocations");
  AssertFatal(lua_isfunction(lua_dl_state, -1),
              "Lua DL script must define compute_dl_allocations() function\n");
  lua_pop(lua_dl_state, 1);

  LOG_I(NR_MAC, "Lua DL scheduler loaded from '%s'\n", script_path);
}

void reload_lua_dl_scheduler(const char *script_path)
{
  pthread_mutex_lock(&lua_dl_mutex);

  if (lua_dl_state) {
    lua_close(lua_dl_state);
    lua_dl_state = NULL;
  }

  lua_dl_state = luaL_newstate();
  if (!lua_dl_state) {
    LOG_E(NR_MAC, "Lua DL reload: failed to create Lua state\n");
    pthread_mutex_unlock(&lua_dl_mutex);
    return;
  }
  luaL_openlibs(lua_dl_state);

  int err = luaL_dofile(lua_dl_state, script_path);
  if (err) {
    LOG_E(NR_MAC, "Lua DL reload: failed to load '%s': %s\n",
          script_path, lua_tostring(lua_dl_state, -1));
    lua_close(lua_dl_state);
    lua_dl_state = NULL;
    pthread_mutex_unlock(&lua_dl_mutex);
    return;
  }

  lua_getglobal(lua_dl_state, "compute_dl_allocations");
  if (!lua_isfunction(lua_dl_state, -1)) {
    LOG_E(NR_MAC, "Lua DL reload: compute_dl_allocations() not found in '%s'\n", script_path);
    lua_close(lua_dl_state);
    lua_dl_state = NULL;
    pthread_mutex_unlock(&lua_dl_mutex);
    return;
  }
  lua_pop(lua_dl_state, 1);

  LOG_I(NR_MAC, "Lua DL scheduler reloaded from '%s'\n", script_path);
  pthread_mutex_unlock(&lua_dl_mutex);
}

void nr_dl_lua_policy(const nr_dl_sched_params_t *params,
                      const nr_dl_candidate_t *candidates,
                      nr_dl_alloc_t *allocs,
                      int n_candidates)
{
  if (!lua_dl_state || n_candidates == 0)
    return;

  /* Convert candidates to dl_ue_metric_t array */
  dl_ue_metric_t metrics[MAX_MOBILES_PER_GNB];
  memset(metrics, 0, sizeof(metrics));
  memset(allocs, 0, n_candidates * sizeof(allocs[0]));

  for (int i = 0; i < n_candidates; i++) {
    const nr_dl_candidate_t *cand = &candidates[i];
    dl_ue_metric_t *m = &metrics[i];

    m->rnti = cand->UE->rnti;
    m->nr_of_layers = cand->nrOfLayers;
    m->pending_bytes = cand->pending_bytes;
    m->throughput = cand->avg_throughput;
    m->previous_mcs = cand->current_mcs;
    m->mcs_table = cand->mcs_table;
    m->bler = cand->bler;
    m->frame = params->frame;
    m->slot = params->slot;
    m->bwp_start = cand->bwp_start;
    m->bwp_size = cand->bwp_size;
    m->uid = cand->UE->uid;
    m->cqi = cand->UE->UE_sched_ctrl.CSI_report.cri_ri_li_pmi_cqi_report.wb_cqi_1tb;
    m->dl_rsrp = cand->UE->mac_stats.num_rsrp_meas > 0
                     ? cand->UE->mac_stats.cumul_rsrp / cand->UE->mac_stats.num_rsrp_meas
                     : 0;

    /* fiveQI: extract from first DRB's QoS config */
    NR_UE_sched_ctrl_t *sc = &cand->UE->UE_sched_ctrl;
    m->fiveQI = 0;
    for (int j = 0; j < seq_arr_size(&sc->lc_config); j++) {
      const nr_lc_config_t *c = seq_arr_at(&sc->lc_config, j);
      if (c->lcid >= 4) {
        for (int q = 0; q < NR_MAX_NUM_QFI; q++) {
          if (c->qos_config[q].fiveQI > 0) {
            m->fiveQI = c->qos_config[q].fiveQI;
            break;
          }
        }
        if (m->fiveQI > 0) break;
      }
    }

    /* HOL delay: not available in this fork (needs RLC extension) */
    m->hol_delay_us = 0;

    if (cand->is_retx) {
      m->ue_type = 1;
      m->required_rbs = cand->retx_rbSize;
      m->required_mcs = cand->current_mcs;
    } else {
      m->ue_type = 0;
      m->required_rbs = 0;
      m->required_mcs = 255;
    }
  }

  /* Build RB mask string from VRB map */
  char rb_mask[MAX_BWP_SIZE + 1];
  for (int rb = 0; rb < MAX_BWP_SIZE; rb++) {
    rb_mask[rb] = (params->vrb_map[rb] & params->slbitmap) ? 'X' : '.';
  }
  rb_mask[MAX_BWP_SIZE] = '\0';

  /* Call Lua */
  pthread_mutex_lock(&lua_dl_mutex);
  if (!lua_dl_state) {
    pthread_mutex_unlock(&lua_dl_mutex);
    return;
  }

  lua_getglobal(lua_dl_state, "compute_dl_allocations");
  lua_pushlightuserdata(lua_dl_state, metrics);
  lua_pushinteger(lua_dl_state, n_candidates);
  lua_pushinteger(lua_dl_state, params->n_rb_avail);
  lua_pushinteger(lua_dl_state, 5); /* min_rb */
  lua_pushstring(lua_dl_state, rb_mask);

  int err = lua_pcall(lua_dl_state, 5, 0, 0);
  if (err) {
    LOG_E(NR_MAC, "Lua DL scheduler error: %s\n", lua_tostring(lua_dl_state, -1));
    lua_pop(lua_dl_state, 1);
    pthread_mutex_unlock(&lua_dl_mutex);
    return;
  }
  pthread_mutex_unlock(&lua_dl_mutex);

  /* Read back allocations */
  for (int i = 0; i < n_candidates; i++) {
    if (metrics[i].allocated_rb > 0) {
      allocs[i].scheduled = true;
      allocs[i].rbStart = metrics[i].allocated_rb_start;
      allocs[i].rbSize = metrics[i].allocated_rb;
      allocs[i].mcs = metrics[i].allocated_mcs;
    }
  }
}

void set_lua_dl_scheduler_config(int fwa_max_throughput, int mtc_max_throughput)
{
  pthread_mutex_lock(&lua_dl_mutex);
  if (!lua_dl_state) {
    LOG_E(NR_MAC, "Lua DL scheduler not initialized, cannot set config\n");
    pthread_mutex_unlock(&lua_dl_mutex);
    return;
  }

  lua_pushinteger(lua_dl_state, fwa_max_throughput);
  lua_setglobal(lua_dl_state, "fwa_max_throughput");

  lua_pushinteger(lua_dl_state, mtc_max_throughput);
  lua_setglobal(lua_dl_state, "mtc_max_throughput");

  pthread_mutex_unlock(&lua_dl_mutex);

  LOG_I(NR_MAC, "Lua DL scheduler config: fwa_max_throughput=%d Mbps, mtc_max_throughput=%d Mbps\n",
        fwa_max_throughput, mtc_max_throughput);
}
