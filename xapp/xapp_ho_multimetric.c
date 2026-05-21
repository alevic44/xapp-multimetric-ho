/*
 * xApp per handover multi-metrica tra due gNB
 *
 * Contribuzione scientifica:
 *   - Raccoglie RSRP (reale, SSB CSI path), SINR (reale, dopo fix SINR_index),
 *     throughput DL/UL, PRB usage via KPM
 *   - RSRQ conforme 3GPP via RRC MeasurementReport (path RC, conversione: (raw-87)/2.0)
 *   - Algoritmo di decisione con scoring pesato multi-criterio
 *   - Anti-ping-pong: hysteresis timer (5s min tra HO) + N=3 campioni consecutivi
 *   - CSV logging completo per analisi offline e sezione ML dell'articolo
 *
 * Setup:
 *   Source gNB B210  - Global ID=0x12345, nr_cellid=12345678L
 *   Target gNB N310  - Global ID=0x002,   nr_cellid=12345679L
 */

#include "../../../../src/xApp/e42_xapp_api.h"
#include "../../../../src/util/time_now_us.h"
#include "../../../../src/util/alg_ds/ds/lock_guard/lock_guard.h"
#include "../../../../src/util/e.h"
#include "../../../../src/util/byte_array.h"
#include "../../../../src/util/conversions.h"
#include "../../../../src/sm/kpm_sm/kpm_sm_v02.03/ie/kpm_data_ie.h"
#include "../../../../src/sm/kpm_sm/kpm_sm_v02.03/ie/kpm_data_ie/kpm_ric_info/kpm_ric_ind_msg_frm_1.h"
#include "../../../../src/sm/kpm_sm/kpm_sm_v02.03/ie/kpm_data_ie/data/meas_data_lst.h"
#include "../../../../src/sm/kpm_sm/kpm_sm_v02.03/ie/kpm_data_ie/data/meas_info_frm_1_lst.h"
#include "../../../../src/sm/rc_sm/ie/rc_data_ie.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_func_def_ctrl.h"
#include "../../../../src/sm/rc_sm/ie/ir/seq_ctrl_style.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_struct.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_type.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_list.h"
#include "../../../../src/sm/rc_sm/ie/ir/lst_ran_param.h"
#include "../../../../src/lib/sm/ie/ue_id.h"
#include "../../../../src/sm/rc_sm/ie/ir/seq_ue_id.h"
#include "../../../../src/sm/rc_sm/ie/ir/ue_info_chng.h"
#include "../../../../src/sm/rc_sm/ie/ir/rrc_state.h"
#include "../../../../src/sm/rc_sm/ie/ir/param_report_def.h"
#include "NR_UL-DCCH-Message.h"
#include "NR_MeasResults.h"
#include "../../../../src/lib/sm/dec/dec_ue_id.c"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

// ──────────────────────────────────────────────
// Costanti di configurazione
// ──────────────────────────────────────────────

#define MAX_UE_COUNT       1000
#define MAX_NODES          10
#define PERIOD_MS          100    // Periodo KPM in ms

// Anti-ping-pong: intervallo minimo tra due HO consecutivi per lo stesso UE
#define MIN_HO_INTERVAL_US 5000000LL   // 5 secondi in microsecondi

// Anti-ping-pong: numero minimo di campioni consecutivi sotto soglia prima del trigger
#define HO_HYSTERESIS_SAMPLES 3

// Warm-up: numero di campioni KPM iniziali da ignorare (skip garbage data)
#define KPM_WARMUP_SAMPLES 2

// Soglia di scoring (default — sovrascrivibile con --threshold)
#define score_threshold_DEFAULT  0.30
static double score_threshold = score_threshold_DEFAULT;

// ──────────────────────────────────────────────
// Struttura pesi per lo scoring multi-metrica
// I pesi sono motivati dal training ML offline
// (feature importance di un Random Forest addestrato
//  su dati raccolti con questa stessa xApp in modalità
//  monitoring-only, come descritto nella Sezione V
//  dell'articolo scientifico).
// ──────────────────────────────────────────────
typedef struct {
    double w_rsrp;   // peso RSRP (qualità segnale SS)
    double w_rsrq;   // peso RSRQ (Reference Signal Received Quality = signal + interference)
    double w_thp;    // peso throughput (DL+UL combinato)
    double w_prb;    // peso PRB usage (congestione, contributo inverso)
} scoring_weights_t;

static scoring_weights_t weights = {
    .w_rsrp = 0.20,
    .w_rsrq = 0.20,
    .w_thp  = 0.60,
    .w_prb  = 0.10,
};

// ──────────────────────────────────────────────
// Struttura per lo stato per-UE
// ──────────────────────────────────────────────
typedef struct {
    uint64_t ue_id;
    uint32_t gnb_id;       // gNB ID from node_idx (source vs target)

    // Metriche KPM (path gNB MAC/PHY)
    double rsrp;           // dBm  – reale (SSB CSI lookup table 3GPP 38.133)
    double sinr;           // dB   – reale dopo fix SINR_index, range [-32, +31.5]
    double throughput_dl;  // kbps
    double throughput_ul;  // kbps
    double prb_tot_dl;     // PRB count
    double rlc_delay_dl;   // media delay RLC DL (us)

    // Metriche RC (path RRC MeasurementReport dall'UE)
    // Conversioni secondo TS 38.133:
    //   rsrp_rrc = raw - 156  [dBm]
    //   rsrq_rrc = (raw - 87) / 2.0  [dB]
    //   sinr_rrc = (raw - 46) / 2.0  [dB]
    double rsrp_rrc;
    double rsrq_rrc;
    double sinr_rrc;
    bool   rrc_metrics_valid;  // true se almeno un MeasReport è stato ricevuto

    // Score corrente e stato HO
    double score;
    int    score_below_threshold_count;  // campioni consecutivi sotto score_threshold
    bool   handover_triggered;
    int64_t last_handover_time;
    int    ho_count;

    int64_t last_update;
    bool    valid;
    int     kpm_samples_received;  // skip primi N campioni KPM (warm-up)
} ue_metrics_t;

// ──────────────────────────────────────────────
// Variabili globali
// ──────────────────────────────────────────────
static ue_metrics_t      ue_metrics_array[MAX_UE_COUNT];
static pthread_mutex_t   metrics_mutex;
static e2_node_arr_xapp_t *global_nodes = NULL;
static int64_t            xapp_start_time = 0;

// CSV — Singolo file unificato (KPM + RRC)
static FILE             *csv_file = NULL;
static pthread_mutex_t   csv_mutex;

// ──────────────────────────────────────────────
// Forward declarations
// ──────────────────────────────────────────────
static void init_csv_logging(void);
static void log_csv_row(const ue_metrics_t *m, bool ho_triggered);
static void close_csv_logging(void);
static void send_handover_control_command(uint64_t ue_id, int serving_node_idx);
static void force_handover_now(void);
static rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const *ctrl_def, uint64_t amf_ue_ngap_id, int target_gnb_idx);
static void configure_target_gnb(seq_ran_param_t *nr_cgi_leaf_param, int target_gnb_idx);

// ──────────────────────────────────────────────
// Normalizzazione metriche su [0,1] (range 3GPP)
// ──────────────────────────────────────────────

// clamp helper
static inline double clamp(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// RSRP: range [-140, -44] dBm
static inline double norm_rsrp(double rsrp) {
    return (clamp(rsrp, -140.0, -44.0) - (-140.0)) / (-44.0 - (-140.0));
}

// RSRQ: range [-43, 20] dB (TS 38.133 Table 10.1.5.1-1)
// More sensitive to channel degradation in lab than SINR (which is saturated when source is near)
static inline double norm_rsrq(double rsrq) {
    return (clamp(rsrq, -43.0, 20.0) - (-43.0)) / (20.0 - (-43.0));
}

// Throughput combinato DL+UL: range [0, 20000] kbps (20 Mbps max)
static inline double norm_thp(double thp_dl, double thp_ul) {
    return clamp((thp_dl + thp_ul) / 2.0, 0.0, 20000.0) / 20000.0;
}

// PRB usage: range [0, 106] (max PRB in 40 MHz NR)
// Contributo inverso: più PRB usati = peggio = score più basso
static inline double norm_prb_inverse(double prb) {
    return 1.0 - clamp(prb, 0.0, 106.0) / 106.0;
}

// ──────────────────────────────────────────────
// Calcolo score multi-metrica [0, 1]
// Score più alto = canale migliore
// ──────────────────────────────────────────────
static double compute_score(const ue_metrics_t *m) {
    // Use RC (RRC MeasReport) metrics when available, fallback to KPM
    double rsrp_eff = m->rrc_metrics_valid ? m->rsrp_rrc : m->rsrp;
    double rsrq_eff = m->rrc_metrics_valid ? m->rsrq_rrc : -12.0;  // fallback: poor quality

    double n_rsrp = norm_rsrp(rsrp_eff);
    double n_rsrq = norm_rsrq(rsrq_eff);
    double n_thp  = norm_thp(m->throughput_dl, m->throughput_ul);
    double n_prb  = norm_prb_inverse(m->prb_tot_dl);

    return weights.w_rsrp * n_rsrp
         + weights.w_rsrq * n_rsrq
         + weights.w_thp  * n_thp
         + weights.w_prb  * n_prb;
}

// ──────────────────────────────────────────────
// Logica di decisione handover
// Restituisce true se il trigger HO deve essere inviato
// ──────────────────────────────────────────────
// Debug: trigger HO manuale bypassando scoring
// Usare premendo 'h' + Enter nel terminale
// ──────────────────────────────────────────────
static void force_handover_now(void) {
    pthread_mutex_lock(&metrics_mutex);
    for (int i = 0; i < MAX_UE_COUNT; i++) {
        ue_metrics_t *m = &ue_metrics_array[i];
        if (!m->valid) continue;
        printf("[DEBUG HO] Forcing handover for UE %lu serving=node%d\n", m->ue_id, m->gnb_id);
        uint64_t ue_id = m->ue_id;
        int serving = m->gnb_id;
        m->handover_triggered = true;
        m->last_handover_time = time_now_us();
        m->ho_count++;
        m->score_below_threshold_count = 0;
        pthread_mutex_unlock(&metrics_mutex);
        // Chiamata fuori dal lock per evitare deadlock con callback KPM
        send_handover_control_command(ue_id, serving);
        return;
    }
    pthread_mutex_unlock(&metrics_mutex);
    printf("[DEBUG HO] No valid UE found in metrics array - wait for KPM indications\n");
}

static void *debug_input_thread(void *arg) {
    (void)arg;
    printf("[DEBUG] Press 'h' + Enter to force immediate handover (bypasses scoring)\n");
    char buf[64];
    while (fgets(buf, sizeof(buf), stdin) != NULL) {
        if (buf[0] == 'h' || buf[0] == 'H') {
            printf("[DEBUG HO] Manual HO trigger requested!\n");
            force_handover_now();
        }
    }
    return NULL;
}

// ──────────────────────────────────────────────
// UE tracking helpers
// ──────────────────────────────────────────────
static uint64_t hash_ue_id(uint64_t id) { return id % MAX_UE_COUNT; }

static ue_metrics_t *get_ue_metrics(uint64_t id) {
    return &ue_metrics_array[hash_ue_id(id)];
}

static void init_ue_metrics(uint64_t id) {
    ue_metrics_t *m = get_ue_metrics(id);
    memset(m, 0, sizeof(*m));
    m->ue_id = id;
    m->rsrp  = -120.0;
    m->sinr  = 0.0;
    m->rsrq_rrc = -12.0;
    m->rsrp_rrc = -120.0;
    m->sinr_rrc = 0.0;
}

// ──────────────────────────────────────────────
// Decodifica RRC MeasurementReport da RC Indication
// Aggiorna rsrp_rrc, rsrq_rrc, sinr_rrc nell'UE
// (Conversioni conformi TS 38.133)
// ──────────────────────────────────────────────
static void decode_rrc_meas_report(const byte_array_t octet_str, ue_metrics_t *m) {
    NR_UL_DCCH_Message_t *msg = NULL;
    asn_dec_rval_t dec = uper_decode(NULL, &asn_DEF_NR_UL_DCCH_Message,
                                     (void **)&msg,
                                     octet_str.buf, octet_str.len, 0, 0);
    if (dec.code != RC_OK || msg == NULL) return;

    if (msg->message.present != NR_UL_DCCH_MessageType_PR_c1) goto out;
    if (msg->message.choice.c1->present !=
        NR_UL_DCCH_MessageType__c1_PR_measurementReport) goto out;

    NR_MeasResults_t *results =
        &msg->message.choice.c1->choice.measurementReport
             ->criticalExtensions.choice.measurementReport->measResults;

    if (results == NULL) goto out;

    for (int i = 0; i < results->measResultServingMOList.list.count; i++) {
        NR_MeasResultServMO_t *servmo =
            results->measResultServingMOList.list.array[i];
        NR_MeasQuantityResults_t *mqr =
            servmo->measResultServingCell.measResult.cellResults.resultsSSB_Cell;

        if (mqr == NULL) continue;

        if (mqr->rsrp) {
            // TS 38.133 Table 10.1.6.1-1: RSRP_dBm = raw - 156
            m->rsrp_rrc = (double)(*mqr->rsrp) - 156.0;
        }
        if (mqr->rsrq) {
            // TS 38.133 Table 10.1.6.1-1: RSRQ_dB = (raw - 87) / 2.0
            m->rsrq_rrc = ((double)(*mqr->rsrq) - 87.0) / 2.0;
        }
        if (mqr->sinr) {
            // TS 38.133 Table 10.1.16.1-1: SINR_dB = (raw - 46) / 2.0
            m->sinr_rrc = ((double)(*mqr->sinr) - 46.0) / 2.0;
        }
        m->rrc_metrics_valid = true;

        printf("[xApp HO] UE %lu - RRC MeasReport:"
               " RSRP=%.1f dBm, RSRQ=%.1f dB, SINR=%.1f dB\n",
               m->ue_id, m->rsrp_rrc, m->rsrq_rrc, m->sinr_rrc);
        break; // Prendi solo la serving cell
    }

out:
    ASN_STRUCT_FREE(asn_DEF_NR_UL_DCCH_Message, msg);
}

// ──────────────────────────────────────────────
// Generazione subscription KPM
// ──────────────────────────────────────────────
static test_info_lst_t filter_predicate(test_cond_type_e type,
                                        test_cond_e cond,
                                        int value)
{
    (void)value;
    test_info_lst_t dst = {0};
    dst.test_cond_type = type;
    dst.S_NSSAI = TRUE_TEST_COND_TYPE;
    dst.test_cond = calloc(1, sizeof(test_cond_e));
    assert(dst.test_cond != NULL);
    *dst.test_cond = cond;
    dst.test_cond_value = calloc(1, sizeof(test_cond_value_t));
    assert(dst.test_cond_value != NULL);
    dst.test_cond_value->type = OCTET_STRING_TEST_COND_VALUE;
    dst.test_cond_value->octet_string_value = calloc(1, sizeof(byte_array_t));
    assert(dst.test_cond_value->octet_string_value != NULL);
    const size_t len = 4;
    dst.test_cond_value->octet_string_value->len = len;
    dst.test_cond_value->octet_string_value->buf = calloc(len, sizeof(uint8_t));
    assert(dst.test_cond_value->octet_string_value->buf != NULL);
    dst.test_cond_value->octet_string_value->buf[0] = 1;  // SST=1
    dst.test_cond_value->octet_string_value->buf[1] = 0x00;
    dst.test_cond_value->octet_string_value->buf[2] = 0x00;
    dst.test_cond_value->octet_string_value->buf[3] = 0x01; // SD=1
    return dst;
}

static kpm_sub_data_t gen_kpm_subs(kpm_ran_function_def_t const *ran_func)
{
    assert(ran_func != NULL);
    assert(ran_func->ric_event_trigger_style_list != NULL);

    kpm_sub_data_t kpm_sub = {0};
    kpm_sub.ev_trg_def.type = FORMAT_1_RIC_EVENT_TRIGGER;
    kpm_sub.ev_trg_def.kpm_ric_event_trigger_format_1.report_period_ms = PERIOD_MS;

    kpm_sub.sz_ad = 1;
    kpm_sub.ad = calloc(1, sizeof(kpm_act_def_t));
    assert(kpm_sub.ad != NULL);

    ric_report_style_item_t *report_item = &ran_func->ric_report_style_list[0];
    kpm_sub.ad[0].type = FORMAT_4_ACTION_DEFINITION;

    kpm_sub.ad[0].frm_4.matching_cond_lst_len = 1;
    kpm_sub.ad[0].frm_4.matching_cond_lst =
        calloc(1, sizeof(matching_condition_format_4_lst_t));
    kpm_sub.ad[0].frm_4.matching_cond_lst[0].test_info_lst =
        filter_predicate(S_NSSAI_TEST_COND_TYPE, EQUAL_TEST_COND, 1);

    size_t sz = report_item->meas_info_for_action_lst_len;
    printf("[KPM Sub] gNB exposes %zu metrics:", sz);
    for (size_t i = 0; i < sz; ++i) {
        printf(" %.*s", (int)report_item->meas_info_for_action_lst[i].name.len,
               report_item->meas_info_for_action_lst[i].name.buf);
    }
    printf("\n");

    kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst_len = sz;
    kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst =
        calloc(sz, sizeof(meas_info_format_1_lst_t));

    for (size_t i = 0; i < sz; ++i) {
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i]
            .meas_type.type = NAME_MEAS_TYPE;
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i]
            .meas_type.name =
            copy_byte_array(report_item->meas_info_for_action_lst[i].name);

        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i]
            .label_info_lst_len = 1;
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i]
            .label_info_lst = calloc(1, sizeof(label_info_lst_t));
        assert(kpm_sub.ad[0].frm_4.action_def_format_1
                   .meas_info_lst[i].label_info_lst != NULL);
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i]
            .label_info_lst[0].noLabel = calloc(1, sizeof(enum_value_e));
        assert(kpm_sub.ad[0].frm_4.action_def_format_1
                   .meas_info_lst[i].label_info_lst[0].noLabel != NULL);
        *kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i]
            .label_info_lst[0].noLabel = TRUE_ENUM_VALUE;
    }
    kpm_sub.ad[0].frm_4.action_def_format_1.gran_period_ms = PERIOD_MS;
    kpm_sub.ad[0].frm_4.action_def_format_1.cell_global_id = NULL;

    return kpm_sub;
}

// ──────────────────────────────────────────────
// Callback KPM: raccolta metriche e decisione HO
// ──────────────────────────────────────────────
static void sm_cb_kpm_internal(sm_ag_if_rd_t const *rd, int node_idx)
{
    assert(rd != NULL);
    const kpm_ind_data_t *ind = &rd->ind.kpm.ind;
    const kpm_ind_msg_format_3_t *msg_frm_3 = &ind->msg.frm_3;
    static int counter = 1;

    printf("\n[%d] KPM indication received from node %d (%zu UEs)\n",
           counter++, node_idx, msg_frm_3->ue_meas_report_lst_len);

    int64_t now = time_now_us();
    pthread_mutex_lock(&metrics_mutex);

    for (size_t i = 0; i < msg_frm_3->ue_meas_report_lst_len; i++) {
        const kpm_ind_msg_format_1_t *msg =
            &msg_frm_3->meas_report_per_ue[i].ind_msg_format_1;

        uint64_t ue_id = 0;
        if (msg_frm_3->meas_report_per_ue[i].ue_meas_report_lst.type ==
            GNB_UE_ID_E2SM) {
            ue_id = msg_frm_3->meas_report_per_ue[i]
                        .ue_meas_report_lst.gnb.amf_ue_ngap_id;
        } else {
            ue_id = i + 1;
        }

        ue_metrics_t *m = get_ue_metrics(ue_id);
        if (!m->valid) {
            init_ue_metrics(ue_id);
        }
        m->gnb_id = node_idx;  // Track which gNB sent this data

        // Print received metric names on first indication only
        if (counter == 2 && i == 0) {
            printf("[KPM Ind] Received %zu metrics:", msg->meas_info_lst_len);
            for (size_t mi = 0; mi < msg->meas_info_lst_len; mi++) {
                const meas_info_format_1_lst_t *inf = &msg->meas_info_lst[mi];
                if (inf->meas_type.type == NAME_MEAS_TYPE)
                    printf(" %.*s", (int)inf->meas_type.name.len, inf->meas_type.name.buf);
            }
            printf("\n");
        }

        // Parsing metriche KPM
        for (size_t j = 0; j < msg->meas_data_lst_len; ++j) {
            const meas_data_lst_t *data = &msg->meas_data_lst[j];
            for (size_t k = 0;
                 k < data->meas_record_len && k < msg->meas_info_lst_len;
                 ++k) {
                const meas_info_format_1_lst_t *info = &msg->meas_info_lst[k];
                if (info->meas_type.type != NAME_MEAS_TYPE) continue;

                if (cmp_str_ba("RSRP", info->meas_type.name) == 0) {
                    // RSRP è INTEGER_MEAS_VALUE (int dBm)
                    m->rsrp = (double)data->meas_record_lst[k].int_val;
                } else if (cmp_str_ba("SINR", info->meas_type.name) == 0) {
                    // SINR è ora REAL_MEAS_VALUE in dB (dopo fix ran_func_kpm_subs.c)
                    m->sinr = data->meas_record_lst[k].real_val;
                } else if (cmp_str_ba("DRB.UEThpDl", info->meas_type.name) == 0) {
                    m->throughput_dl = data->meas_record_lst[k].real_val;
                } else if (cmp_str_ba("DRB.UEThpUl", info->meas_type.name) == 0) {
                    m->throughput_ul = data->meas_record_lst[k].real_val;
                } else if (cmp_str_ba("RRU.PrbTotDl", info->meas_type.name) == 0) {
                    m->prb_tot_dl = (double)data->meas_record_lst[k].int_val;
                } else if (cmp_str_ba("DRB.RlcSduDelayDl", info->meas_type.name) == 0) {
                    m->rlc_delay_dl = data->meas_record_lst[k].real_val;
                }
            }
        }

        m->last_update = now;
        m->valid = true;
        m->kpm_samples_received++;

        // Calcola score SEMPRE (prima di controllare warmup)
        m->score = compute_score(m);

        // Skip primi KPM_WARMUP_SAMPLES campioni (garbage data warm-up)
        bool in_warmup = (m->kpm_samples_received <= KPM_WARMUP_SAMPLES);

        // Valuta HO solo dopo warmup E solo quando RC MeasReport è arrivato
        // (score è inaffidabile senza RC: RSRP=-120 e SINR=0 da KPM)
        bool trigger = false;
        if (!in_warmup && m->rrc_metrics_valid) {
            // Anti-ping-pong: blocca se HO recente
            bool ho_cooldown = m->handover_triggered &&
                               (now - m->last_handover_time) < MIN_HO_INTERVAL_US;
            if (!ho_cooldown) {
                if (m->score < score_threshold) {
                    m->score_below_threshold_count++;
                } else {
                    m->score_below_threshold_count = 0;
                }
                trigger = m->score_below_threshold_count >= HO_HYSTERESIS_SAMPLES;
            }
        }

        if (trigger) {
            m->handover_triggered = true;
            m->last_handover_time = now;
            m->ho_count++;
            m->score_below_threshold_count = 0;
        }
    }

    pthread_mutex_unlock(&metrics_mutex);

    // Log, print e send HO FUORI dal mutex
    // (Riacquisiamo snapshot già copiato sopra — iteriamo di nuovo per ogni UE)
    pthread_mutex_lock(&metrics_mutex);
    for (size_t i = 0; i < msg_frm_3->ue_meas_report_lst_len; i++) {
        uint64_t ue_id2 = 0;
        if (msg_frm_3->meas_report_per_ue[i].ue_meas_report_lst.type == GNB_UE_ID_E2SM)
            ue_id2 = msg_frm_3->meas_report_per_ue[i].ue_meas_report_lst.gnb.amf_ue_ngap_id;
        else
            ue_id2 = i + 1;

        ue_metrics_t *m2 = get_ue_metrics(ue_id2);
        if (!m2->valid) continue;
        ue_metrics_t snap = *m2;
        pthread_mutex_unlock(&metrics_mutex);

        double rsrp_eff = snap.rrc_metrics_valid ? snap.rsrp_rrc : snap.rsrp;
        double sinr_eff = snap.rrc_metrics_valid ? snap.sinr_rrc : snap.sinr;
        bool was_warmup = (snap.kpm_samples_received <= KPM_WARMUP_SAMPLES);

        printf("[HO Score] UE %lu | RSRP=%.1f%s | SINR=%.1f%s |"
               " Thp=%.0f+%.0f kbps | PRB=%.0f |"
               " score=%.3f %s | consec=%d | HO#%d\n",
               snap.ue_id, rsrp_eff, snap.rrc_metrics_valid ? "(RC)" : "(KPM)",
               sinr_eff, snap.rrc_metrics_valid ? "(RC)" : "(KPM)",
               snap.throughput_dl, snap.throughput_ul, snap.prb_tot_dl,
               snap.score,
               was_warmup ? "[WARMUP]" : (snap.score < score_threshold ? "[LOW]" : "[OK]"),
               snap.score_below_threshold_count,
               snap.ho_count);

        if (snap.rrc_metrics_valid) {
            printf("[HO Score] UE %lu | RRC: RSRP=%.1f dBm"
                   " RSRQ=%.1f dB SINR=%.1f dB\n",
                   snap.ue_id, snap.rsrp_rrc, snap.rsrq_rrc, snap.sinr_rrc);
        }

        log_csv_row(&snap, snap.handover_triggered && snap.ho_count > 0 &&
                           snap.score_below_threshold_count == 0);

        if (snap.handover_triggered && snap.last_handover_time == now) {
            printf("[HO Decision] UE %lu - TRIGGERING HANDOVER (score=%.3f"
                   " < %.3f, %d campioni consecutivi) serving=node%d\n",
                   snap.ue_id, snap.score, score_threshold,
                   HO_HYSTERESIS_SAMPLES, snap.gnb_id);
            send_handover_control_command(snap.ue_id, snap.gnb_id);
        }

        pthread_mutex_lock(&metrics_mutex);
    }
    pthread_mutex_unlock(&metrics_mutex);
}

// Wrapper callbacks per-nodo (la callback sm_cb non supporta user data)
static void sm_cb_kpm_node0(sm_ag_if_rd_t const *rd) { sm_cb_kpm_internal(rd, 0); }
static void sm_cb_kpm_node1(sm_ag_if_rd_t const *rd) { sm_cb_kpm_internal(rd, 1); }
static sm_cb kpm_cb_per_node[MAX_NODES] = { sm_cb_kpm_node0, sm_cb_kpm_node1 };

// ──────────────────────────────────────────────
// Callback RC: decodifica RRC MeasurementReport
// Aggiorna RSRP/RSRQ/SINR UE-side (conformi 3GPP)
// ──────────────────────────────────────────────
static void sm_cb_rc(sm_ag_if_rd_t const *rd)
{
    assert(rd != NULL);
    if (rd->type != INDICATION_MSG_AGENT_IF_ANS_V0) return;

    const rc_ind_data_t *rc_ind = &rd->ind.rc.ind;
    if (rc_ind->hdr.format != FORMAT_1_E2SM_RC_IND_HDR) return;
    if (rc_ind->msg.format != FORMAT_1_E2SM_RC_IND_MSG) return;

    const e2sm_rc_ind_hdr_frmt_1_t *hdr = &rc_ind->hdr.frmt_1;
    const e2sm_rc_ind_msg_frmt_1_t *msg = &rc_ind->msg.frmt_1;
    (void)hdr;

    static int rc_counter = 1;
    printf("\n[RC %d] RC Indication received (params=%zu)\n",
           rc_counter++, msg->sz_seq_ran_param);

    // Fase 1: Trova UE id e octet_string FUORI dal mutex
    pthread_mutex_lock(&metrics_mutex);
    uint64_t ue_id = 0;
    for (int idx = 0; idx < MAX_UE_COUNT; idx++) {
        if (ue_metrics_array[idx].valid && ue_metrics_array[idx].ue_id > 0) {
            ue_id = ue_metrics_array[idx].ue_id;
            break;
        }
    }
    if (ue_id == 0) ue_id = 1;
    pthread_mutex_unlock(&metrics_mutex);

    // Fase 2: Decodifica ASN.1 FUORI dal mutex (operazione lenta)
    double rsrp_rrc = -120.0, rsrq_rrc = -12.0, sinr_rrc = 0.0;
    bool decoded = false;
    int param_id_found = 0;

    for (size_t j = 0; j < msg->sz_seq_ran_param; j++) {
        const seq_ran_param_t *p = &msg->seq_ran_param[j];
        param_id_found = p->ran_param_id;

        ran_parameter_value_t *val = NULL;
        if (p->ran_param_val.type == ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE)
            val = p->ran_param_val.flag_true;
        else if (p->ran_param_val.type == ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE)
            val = p->ran_param_val.flag_false;

        if (val != NULL && val->type == OCTET_STRING_RAN_PARAMETER_VALUE) {
            // Decodifica ASN.1 temporanea fuori dal lock
            ue_metrics_t tmp = {0};
            tmp.rsrp_rrc = -120.0; tmp.rsrq_rrc = -12.0; tmp.sinr_rrc = 0.0;
            decode_rrc_meas_report(val->octet_str_ran, &tmp);
            if (tmp.rrc_metrics_valid) {
                rsrp_rrc = tmp.rsrp_rrc;
                rsrq_rrc = tmp.rsrq_rrc;
                sinr_rrc = tmp.sinr_rrc;
                decoded = true;
            }
        }
    }

    // Fase 3: Aggiorna stato UE con mutex (solo scrittura veloce)
    printf("[RC CB] RAN param id=%d per UE %lu\n", param_id_found, ue_id);
    if (decoded) {
        printf("[xApp HO] UE %lu - RRC MeasReport: RSRP=%.1f dBm, RSRQ=%.1f dB, SINR=%.1f dB\n",
               ue_id, rsrp_rrc, rsrq_rrc, sinr_rrc);

        pthread_mutex_lock(&metrics_mutex);
        ue_metrics_t *m = get_ue_metrics(ue_id);
        if (!m->valid) init_ue_metrics(ue_id);
        m->rsrp_rrc = rsrp_rrc;
        m->rsrq_rrc = rsrq_rrc;
        m->sinr_rrc = sinr_rrc;
        m->rrc_metrics_valid = true;
        pthread_mutex_unlock(&metrics_mutex);
    }
}

// ──────────────────────────────────────────────
// Descrittori statici delle due gNB
// node_idx=0: B210  (source) gNB_ID=0x12345, cellID=12345678L, PCI=0
// node_idx=1: B210bis (target) gNB_ID=0x002,  cellID=12345679L, PCI=1
// ──────────────────────────────────────────────
typedef struct {
    uint32_t gnb_id;     // gNB ID a 28 bit (TS 38.473)
    uint64_t cell_id;    // NR Cell Identity
    uint16_t pci;        // Physical Cell ID
} gnb_descriptor_t;

static const gnb_descriptor_t gnb_desc[2] = {
    { .gnb_id = 0x12345, .cell_id = 12345678L, .pci = 0 },  // node 0: B210
    { .gnb_id = 0x002,   .cell_id = 12345679L, .pci = 1 },  // node 1: B210bis
};

// Generazione messaggio RC Control per HO
// target_gnb_idx: indice del gNB destinazione (0=B210, 1=B210bis)
static void configure_target_gnb(seq_ran_param_t *nr_cgi_leaf_param, int target_gnb_idx)
{
    if (nr_cgi_leaf_param == NULL ||
        nr_cgi_leaf_param->ran_param_val.flag_true == NULL ||
        nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf == NULL)
        return;

    assert(target_gnb_idx == 0 || target_gnb_idx == 1);
    const gnb_descriptor_t *tgt = &gnb_desc[target_gnb_idx];

    uint8_t mcc = 1, mnc_high = 0, mnc_low = 1;
    uint8_t *buf = nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf;

    buf[0] = mcc;
    buf[1] = mnc_high;
    buf[2] = mnc_low;
    // gNB_ID: 4 byte big-endian (TS 38.473)
    buf[3] = (tgt->gnb_id >> 24) & 0xFF;
    buf[4] = (tgt->gnb_id >> 16) & 0xFF;
    buf[5] = (tgt->gnb_id >>  8) & 0xFF;
    buf[6] =  tgt->gnb_id        & 0xFF;
    // NR Cell Identity: 4 byte big-endian
    buf[7]  = (tgt->cell_id >> 24) & 0xFF;
    buf[8]  = (tgt->cell_id >> 16) & 0xFF;
    buf[9]  = (tgt->cell_id >>  8) & 0xFF;
    buf[10] =  tgt->cell_id        & 0xFF;
    // PhysicalCellId: 2 byte big-endian
    buf[11] = (tgt->pci >> 8) & 0xFF;
    buf[12] =  tgt->pci       & 0xFF;
    // TAC = 0x0001
    buf[13] = 0x00;
    buf[14] = 0x01;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.len = 15;

    printf("[RC Control] Target NR CGI: MCC=%d MNC=%d%d gNB_ID=0x%05x"
           " CellID=%lu PCI=%d TAC=0x%04x\n",
           mcc, mnc_high, mnc_low, tgt->gnb_id, tgt->cell_id, tgt->pci,
           (buf[13] << 8) | buf[14]);
}

static rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const *ctrl_def,
                                           uint64_t amf_ue_ngap_id,
                                           int target_gnb_idx)
{
    assert(ctrl_def != NULL);
    rc_ctrl_req_data_t rc_ctrl = {0};

    for (size_t i = 0; i < ctrl_def->sz_seq_ctrl_style; i++) {
        if (cmp_str_ba("Connected Mode Mobility Control",
                        ctrl_def->seq_ctrl_style[i].name) != 0)
            continue;

        for (size_t j = 0; j < ctrl_def->seq_ctrl_style[i].sz_seq_ctrl_act; j++) {
            if (cmp_str_ba("Handover Control",
                            ctrl_def->seq_ctrl_style[i].seq_ctrl_act[j].name) != 0)
                continue;

            rc_ctrl.hdr.format = ctrl_def->seq_ctrl_style[i].hdr;
            assert(rc_ctrl.hdr.format == FORMAT_1_E2SM_RC_CTRL_HDR);
            rc_ctrl.hdr.frmt_1.ric_style_type = 3;
            rc_ctrl.hdr.frmt_1.ctrl_act_id    = 1;
            rc_ctrl.hdr.frmt_1.ue_id.type     = GNB_UE_ID_E2SM;
            rc_ctrl.hdr.frmt_1.ue_id.gnb.amf_ue_ngap_id = amf_ue_ngap_id;
            rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_region_id = 0;
            rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_set_id = 0;
            rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_ptr = 0;

            rc_ctrl.msg.format = ctrl_def->seq_ctrl_style[i].msg;
            assert(rc_ctrl.msg.format == FORMAT_1_E2SM_RC_CTRL_MSG);

            rc_ctrl.msg.frmt_1.sz_ran_param = 1;
            rc_ctrl.msg.frmt_1.ran_param = calloc(1, sizeof(seq_ran_param_t));
            assert(rc_ctrl.msg.frmt_1.ran_param != NULL);

            // Target Primary Cell ID (id=1) → Target Cell (id=2) →
            // NR Cell (id=3) → NR CGI (id=4, leaf)
            seq_ran_param_t *p1 = &rc_ctrl.msg.frmt_1.ran_param[0];
            p1->ran_param_id = 1;
            p1->ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
            p1->ran_param_val.strct = calloc(1, sizeof(ran_param_struct_t));
            assert(p1->ran_param_val.strct != NULL);
            p1->ran_param_val.strct->sz_ran_param_struct = 1;
            p1->ran_param_val.strct->ran_param_struct = calloc(1, sizeof(seq_ran_param_t));
            assert(p1->ran_param_val.strct->ran_param_struct != NULL);

            seq_ran_param_t *p2 = &p1->ran_param_val.strct->ran_param_struct[0];
            p2->ran_param_id = 2;
            p2->ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
            p2->ran_param_val.strct = calloc(1, sizeof(ran_param_struct_t));
            assert(p2->ran_param_val.strct != NULL);
            p2->ran_param_val.strct->sz_ran_param_struct = 1;
            p2->ran_param_val.strct->ran_param_struct = calloc(1, sizeof(seq_ran_param_t));
            assert(p2->ran_param_val.strct->ran_param_struct != NULL);

            seq_ran_param_t *p3 = &p2->ran_param_val.strct->ran_param_struct[0];
            p3->ran_param_id = 3;
            p3->ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
            p3->ran_param_val.strct = calloc(1, sizeof(ran_param_struct_t));
            assert(p3->ran_param_val.strct != NULL);
            p3->ran_param_val.strct->sz_ran_param_struct = 1;
            p3->ran_param_val.strct->ran_param_struct = calloc(1, sizeof(seq_ran_param_t));
            assert(p3->ran_param_val.strct->ran_param_struct != NULL);

            seq_ran_param_t *leaf = &p3->ran_param_val.strct->ran_param_struct[0];
            leaf->ran_param_id = 4;
            leaf->ran_param_val.type = ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE;
            leaf->ran_param_val.flag_true = calloc(1, sizeof(ran_parameter_value_t));
            assert(leaf->ran_param_val.flag_true != NULL);
            leaf->ran_param_val.flag_true->type = OCTET_STRING_RAN_PARAMETER_VALUE;
            leaf->ran_param_val.flag_true->octet_str_ran.len = 15;
            leaf->ran_param_val.flag_true->octet_str_ran.buf = calloc(15, 1);
            assert(leaf->ran_param_val.flag_true->octet_str_ran.buf != NULL);

            configure_target_gnb(leaf, target_gnb_idx);
            return rc_ctrl;
        }
    }

    // Stile/azione non trovati
    rc_ctrl.hdr.format = FORMAT_1_E2SM_RC_CTRL_HDR;
    rc_ctrl.hdr.frmt_1.ctrl_act_id = 0; // invalido
    rc_ctrl.msg.format = FORMAT_1_E2SM_RC_CTRL_MSG;
    return rc_ctrl;
}

// serving_node_idx: nodo che sta attualmente servendo l'UE (0 o 1)
/// Il target è sempre il nodo opposto: 1-serving_node_idx
static void send_handover_control_command(uint64_t ue_id, int serving_node_idx) {
    if (global_nodes == NULL) {
        printf("[RC Control] Errore: nodi globali non disponibili\n");
        return;
    }

    int const RC_ran_function = 3;
    int target_gnb_idx = 1 - serving_node_idx;  // opposto del serving

    printf("[RC Control] UE %lu: serving=node%d → target=node%d (%s→%s)\n",
           ue_id, serving_node_idx, target_gnb_idx,
           serving_node_idx == 0 ? "B210" : "B210bis",
           target_gnb_idx   == 0 ? "B210" : "B210bis");

    // Invia il comando RC SOLO al nodo servente (è lui che esegue l'HO)
    if ((size_t)serving_node_idx >= global_nodes->len) {
        printf("[RC Control] Errore: serving_node_idx=%d fuori range\n", serving_node_idx);
        return;
    }

    e2_node_connected_xapp_t *node = &global_nodes->n[serving_node_idx];

    size_t rf_idx = 0;
    bool found_rc = false;
    for (; rf_idx < node->len_rf; rf_idx++) {
        if (node->rf[rf_idx].id == RC_ran_function) {
            found_rc = true;
            break;
        }
    }

    if (!found_rc || node->rf[rf_idx].defn.rc.ctrl == NULL) {
        printf("[RC Control] Nodo %d: RC non disponibile\n", serving_node_idx);
        return;
    }

    rc_ctrl_req_data_t rc_ctrl = gen_rc_ctrl_msg(
        node->rf[rf_idx].defn.rc.ctrl, ue_id, target_gnb_idx);

    if (rc_ctrl.hdr.frmt_1.ctrl_act_id <= 0) {
        printf("[RC Control] Errore: Action ID non valido\n");
        return;
    }

    control_sm_xapp_api(&node->id, RC_ran_function, &rc_ctrl);
    free_rc_ctrl_req_data(&rc_ctrl);
    printf("[RC Control] Comando HO inviato a nodo %d\n", serving_node_idx);

    int nodes_notified = 1;
    if (nodes_notified > 0) {
        printf("[RC Control] Comando handover per UE %lu inviato a %d nodi\n",
               ue_id, nodes_notified);
    } else {
        printf("[RC Control] ERRORE: Nessun nodo con RC supportato trovato\n");
    }
}

// ──────────────────────────────────────────────
// CSV logging — Singolo file unificato (KPM + RRC)
// Colonne: timestamp, ue_id, gnb, rsrp_kpm, sinr_kpm, rsrp_rrc, rsrq_rrc, sinr_rrc,
//          thp_dl, thp_ul, prb_dl, rlc_delay, score, ho_triggered, ho_count
// ──────────────────────────────────────────────
static void init_csv_logging(void)
{
    pthread_mutexattr_t attr = {0};
    int rc = pthread_mutex_init(&csv_mutex, &attr);
    assert(rc == 0);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *ti = localtime(&tv.tv_sec);

    // Create csv/multi/ directory if it doesn't exist
    struct stat st = {0};
    if (stat("csv/multi", &st) == -1) {
        mkdir("csv", 0755);
        mkdir("csv/multi", 0755);
    }

    char fname[256];
    strftime(fname, sizeof(fname), "csv/multi/ho_multi_%d_%H_%M_%S", ti);
    strcat(fname, ".csv");

    csv_file = fopen(fname, "w");
    if (csv_file == NULL) {
        printf("[CSV] Cannot open %s\n", fname);
        return;
    }
    fprintf(csv_file, "# threshold=%.4f\n", score_threshold);
    fprintf(csv_file, "# weights=rsrp:%.4f,rsrq:%.4f,thp:%.4f,prb:%.4f\n",
            weights.w_rsrp, weights.w_rsrq, weights.w_thp, weights.w_prb);
    fprintf(csv_file,
            "timestamp,ue_id,gnb,"
            "rsrp_kpm,sinr_kpm,"
            "rsrp_rrc,rsrq_rrc,sinr_rrc,"
            "thp_dl,thp_ul,prb_dl,rlc_delay,"
            "score,ho_triggered,ho_count\n");
    fflush(csv_file);
    printf("[CSV] Logging to %s (threshold=%.4f)\n", fname, score_threshold);
}

static void log_csv_row(const ue_metrics_t *m, bool ho_triggered)
{
    if (csv_file == NULL) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *ti = localtime(&tv.tv_sec);
    char ts[32];
    strftime(ts, 26, "%Y-%m-%d %H:%M:%S", ti);
    sprintf(ts + strlen(ts), ".%03ld", tv.tv_usec / 1000);

    const char *gnb_label = (m->gnb_id == 0) ? "source" : "target";

    pthread_mutex_lock(&csv_mutex);
    fprintf(csv_file,
            "%s,%lu,%s,"
            "%.2f,%.2f,"
            "%.2f,%.2f,%.2f,"
            "%.1f,%.1f,%.0f,%.2f,"
            "%.4f,%d,%d\n",
            ts, m->ue_id, gnb_label,
            m->rsrp, m->sinr,
            m->rsrp_rrc, m->rsrq_rrc, m->sinr_rrc,
            m->throughput_dl, m->throughput_ul,
            m->prb_tot_dl, m->rlc_delay_dl,
            m->score, (int)ho_triggered, m->ho_count);
    fflush(csv_file);
    pthread_mutex_unlock(&csv_mutex);
}

static void close_csv_logging(void)
{
    pthread_mutex_lock(&csv_mutex);
    if (csv_file != NULL) {
        fclose(csv_file);
        csv_file = NULL;
        printf("[CSV] File closed\n");
    }
    pthread_mutex_unlock(&csv_mutex);
    pthread_mutex_destroy(&csv_mutex);
}

// ──────────────────────────────────────────────
// main
// ──────────────────────────────────────────────
static void print_usage(const char *prog) {
    printf("Usage: %s [flexric options] [--threshold T] [--w-rsrp W] [--w-rsrq W] [--w-thp W] [--w-prb W]\n", prog);
    printf("  --threshold T   Score threshold for HO trigger (default: %.2f)\n", score_threshold_DEFAULT);
    printf("  --w-rsrp W      RSRP weight (default: %.2f)\n", weights.w_rsrp);
    printf("  --w-rsrq W      RSRQ weight (default: %.2f)\n", weights.w_rsrq);
    printf("  --w-thp W       Throughput weight (default: %.2f)\n", weights.w_thp);
    printf("  --w-prb W       PRB (inverse) weight (default: %.2f)\n", weights.w_prb);
}

// Parse custom args, remove them from argv so FlexRIC doesn't choke on them.
// Returns new argc.
static int parse_custom_args(int argc, char *argv[]) {
    int new_argc = 1;  // keep argv[0]
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            score_threshold = atof(argv[++i]);
        } else if (strcmp(argv[i], "--w-rsrp") == 0 && i + 1 < argc) {
            weights.w_rsrp = atof(argv[++i]);
        } else if (strcmp(argv[i], "--w-rsrq") == 0 && i + 1 < argc) {
            weights.w_rsrq = atof(argv[++i]);
        } else if (strcmp(argv[i], "--w-thp") == 0 && i + 1 < argc) {
            weights.w_thp = atof(argv[++i]);
        } else if (strcmp(argv[i], "--w-prb") == 0 && i + 1 < argc) {
            weights.w_prb = atof(argv[++i]);
        } else {
            argv[new_argc++] = argv[i];  // keep unknown args for FlexRIC
        }
    }
    argv[new_argc] = NULL;
    return new_argc;
}

int main(int argc, char *argv[])
{
    setlinebuf(stdout); // flush immediato ad ogni newline anche se rediretto su file
    setenv("XAPP_DURATION", "86400", 1); // 24h

    argc = parse_custom_args(argc, argv);

    fr_args_t args = init_fr_args(argc, argv);
    init_xapp_api(&args);
    sleep(1);

    xapp_start_time = time_now_us();
    printf("[HO MultiMetric]: xApp started at %ld\n", xapp_start_time);
    printf("[HO MultiMetric]: Score threshold=%.2f, hysteresis=%d samples,"
           " min HO interval=%lld s\n",
           score_threshold, HO_HYSTERESIS_SAMPLES,
           (long long)(MIN_HO_INTERVAL_US / 1000000));
    printf("[HO MultiMetric]: Weights:"
           " RSRP=%.3f RSRQ=%.3f Thp=%.2f PRB(inv)=%.2f | Threshold=%.2f\n",
           weights.w_rsrp, weights.w_rsrq,
           weights.w_thp, weights.w_prb, score_threshold);

    e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
    assert(nodes.len > 0);
    global_nodes = &nodes;
    printf("[HO MultiMetric]: Connected E2 nodes = %u\n", nodes.len);

    pthread_mutexattr_t attr = {0};
    int rc = pthread_mutex_init(&metrics_mutex, &attr);
    assert(rc == 0);

    memset(ue_metrics_array, 0, sizeof(ue_metrics_array));
    init_csv_logging();

    int const KPM_ran_function = 2;
    int const RC_ran_function  = 3;

    sm_ans_xapp_t *kpm_handles = calloc(nodes.len, sizeof(sm_ans_xapp_t));
    sm_ans_xapp_t *rc_handles  = calloc(nodes.len, sizeof(sm_ans_xapp_t));
    assert(kpm_handles != NULL && rc_handles != NULL);

    for (size_t i = 0; i < nodes.len; ++i) {
        e2_node_connected_xapp_t *n = &nodes.n[i];

        // KPM subscription
        for (size_t k = 0; k < n->len_rf; k++) {
            if (n->rf[k].id == KPM_ran_function &&
                n->rf[k].defn.kpm.ric_report_style_list != NULL) {
                kpm_sub_data_t kpm_sub = gen_kpm_subs(&n->rf[k].defn.kpm);
                assert(i < MAX_NODES && "Troppi nodi E2, aumentare MAX_NODES");
                kpm_handles[i] = report_sm_xapp_api(
                    &n->id, KPM_ran_function, &kpm_sub, kpm_cb_per_node[i]);
                assert(kpm_handles[i].success == true);
                free_kpm_sub_data(&kpm_sub);
                printf("[HO MultiMetric]: KPM subscription OK for node %zu\n", i);
                break;
            }
        }

        // RC subscription (Event Style 1 – Message Copy) per RRC MeasReport
        // Event Trigger: intercetta UL-DCCH measurementReport (msg_id=1)
        for (size_t k = 0; k < n->len_rf; k++) {
            if (n->rf[k].id == RC_ran_function) {
                rc_sub_data_t rc_sub = {0};
                rc_sub.et.format = FORMAT_1_E2SM_RC_EV_TRIGGER_FORMAT;
                rc_sub.et.frmt_1.sz_msg_ev_trg = 1;
                rc_sub.et.frmt_1.msg_ev_trg = calloc(1, sizeof(msg_ev_trg_t));
                assert(rc_sub.et.frmt_1.msg_ev_trg != NULL);

                msg_ev_trg_t *ev = &rc_sub.et.frmt_1.msg_ev_trg[0];
                memset(ev, 0, sizeof(*ev));
                ev->ev_trigger_cond_id  = 2; // UL-DCCH measurementReport
                ev->msg_type            = RRC_MSG_MSG_TYPE_EV_TRG;
                ev->rrc_msg.type        = NR_RRC_MESSAGE_ID;
                ev->rrc_msg.nr          = UL_DCCH_NR_RRC_CLASS;
                ev->rrc_msg.rrc_msg_id  = 1; // measurementReport

                // Action definition: REPORT Style 1 (Message Copy), FORMAT_1 (param list)
                rc_sub.sz_ad = 1;
                rc_sub.ad = calloc(1, sizeof(e2sm_rc_action_def_t));
                assert(rc_sub.ad != NULL);
                rc_sub.ad[0].ric_style_type = 1; // REPORT Service Style 1: Message Copy
                rc_sub.ad[0].format = FORMAT_1_E2SM_RC_ACT_DEF;
                rc_sub.ad[0].frmt_1.sz_param_report_def = 1;
                rc_sub.ad[0].frmt_1.param_report_def = calloc(1, sizeof(param_report_def_t));
                assert(rc_sub.ad[0].frmt_1.param_report_def != NULL);
                // RAN Param ID 11 = E2SM_RC_RS1_RRC_MESSAGE (RRC Message parameter, Style 1)
                rc_sub.ad[0].frmt_1.param_report_def[0].ran_param_id = E2SM_RC_RS1_RRC_MESSAGE;
                rc_sub.ad[0].frmt_1.param_report_def[0].ran_param_def = NULL;

                rc_handles[i] = report_sm_xapp_api(
                    &n->id, RC_ran_function, &rc_sub, sm_cb_rc);
                if (rc_handles[i].success) {
                    printf("[HO MultiMetric]: RC (MeasReport Style 1) subscription"
                           " OK for node %zu\n", i);
                } else {
                    printf("[HO MultiMetric]: RC subscription FAILED for"
                           " node %zu (RSRQ/SINR_rrc will not be available)\n", i);
                }
                break;
            }
        }
    }

    // Avvia thread per trigger HO manuale da tastiera ('h' + Enter)
    pthread_t debug_thread;
    int drc = pthread_create(&debug_thread, NULL, debug_input_thread, NULL);
    assert(drc == 0);

    xapp_wait_end_api();

    // Termina il thread di input (fgets è cancellation point in glibc)
    pthread_cancel(debug_thread);
    pthread_join(debug_thread, NULL);

    printf("[HO MultiMetric]: Starting cleanup...\n");

    for (size_t i = 0; i < nodes.len; ++i) {
        if (kpm_handles[i].success)
            rm_report_sm_xapp_api(kpm_handles[i].u.handle);
        if (rc_handles[i].success)
            rm_report_sm_xapp_api(rc_handles[i].u.handle);
    }

    close_csv_logging();

    global_nodes = NULL;
    free(kpm_handles);
    free(rc_handles);
    free_e2_node_arr_xapp(&nodes);

    rc = pthread_mutex_destroy(&metrics_mutex);
    assert(rc == 0);

    printf("[HO MultiMetric]: xApp completed successfully\n");
    return 0;
}
