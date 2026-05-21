/*
 * xApp per handover avanzato tra due gNB
 * Setup:
 * - Source: gNB B210 (Core1) - Global ID=0x12345, nr_cellid=12345678L
 * - Target: gNB N310 (Core2) - Global ID=0x002, nr_cellid=12345679L
 * 
 * Comportamento handover:
 * - Il comando di handover viene inviato solo una volta per UE
 * - Dopo 30 secondi dall'ultimo handover, lo stato viene resettato
 * - Questo evita l'invio di comandi duplicati durante condizioni persistenti
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
// Include per RC
#include "../../../../src/sm/rc_sm/ie/rc_data_ie.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_func_def_ctrl.h"
#include "../../../../src/sm/rc_sm/ie/ir/seq_ctrl_style.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_struct.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_type.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_list.h"
#include "../../../../src/sm/rc_sm/ie/ir/lst_ran_param.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_func_def_report.h"
#include "../../../../src/sm/rc_sm/ie/ir/seq_report_sty.h"
#include "../../../../src/sm/rc_sm/ie/ir/msg_ev_trg.h"
#include "../../../../src/sm/rc_sm/ie/ir/param_report_def.h"
#include "../../../../src/lib/sm/ie/ue_id.h"
// Include per decodifica RRC e UEID
#include "NR_UL-DCCH-Message.h"
#include "NR_MeasResults.h"
#include "../../../../src/sm/rc_sm/ie/asn/UEID.h"
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
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>

// Dichiarazioni anticipate per il logging CSV
static void init_csv_logging(void);
static void log_triggering_metric(const char* metric_name, double metric_value);
static void close_csv_logging(void);

// Handler SIGALRM: chiude CSV e termina
static void sigalrm_handler(int sig)
{
    (void)sig;
    printf("\n[xApp] Timeout 30s scaduto, terminazione.\n");
    close_csv_logging();
    exit(0);
}

// Struttura per memorizzare le metriche per UE
typedef struct
{
    uint64_t ue_id;
    double throughput_dl;
    double throughput_ul;
    double packet_loss_rate;
    double pdcp_delay;
    double prb_tot_dl;
    double prb_tot_ul;
    double pdcp_volume_dl;
    double pdcp_volume_ul;
    double rlc_sdu_delay_dl;
    double rsrp;
    double rsrq;
    double sinr;
    int64_t last_update;
    bool valid;
    bool handover_triggered;
    int64_t last_handover_time;
} ue_metrics_t;

// Configurazione soglie per handover
typedef struct
{
    double throughput_threshold;
    double delay_threshold;
} ho_thresholds_t;

static ho_thresholds_t thresholds = {
    .throughput_threshold = 100.0,
    .delay_threshold = 0.20};

#define MAX_UE_COUNT 1000
#define MAX_NODES 10
static ue_metrics_t ue_metrics_array[MAX_UE_COUNT];
static pthread_mutex_t metrics_mutex;

// Variabili globali per il logging CSV
static FILE* csv_file = NULL;
static pthread_mutex_t csv_mutex;

// Array globale per mantenere tutti i nodi connessi
static e2_node_arr_xapp_t* global_nodes = NULL;

// Variabile globale per tracciare il tempo di avvio della xApp
static int64_t xapp_start_time = 0;

// Flag globale: assicura che il comando HO venga inviato una sola volta durante tutta l'esecuzione
static bool global_handover_sent = false;
static pthread_mutex_t ho_sent_mutex = PTHREAD_MUTEX_INITIALIZER;

// Label per identificare source/target gNB nel CSV (indicizzato per nodo)
static char gnb_labels[MAX_NODES][32];

// Etichetta del carico configurata via --label (per distinguere run con iperf diverso)
static char load_label[64] = "default";

// Commento descrittivo del test, configurato via --comment (scritto come prima riga del CSV)
static char csv_comment[256] = "";
static char csv_output_dir[256] = "csv/timer";  // Directory di output CSV (default: csv/timer/)

// Timer HO forzato: se > 0, dopo N secondi l'HO viene triggerato indipendentemente dalle soglie
static int ho_timer_seconds = -1;
static bool timer_ho_triggered = false;
static uint64_t timer_ho_ue_id = 0;  // UE su cui triggerare l'HO da timer

// Thread-local: label del gNB corrente (impostato dal wrapper callback prima di chiamare sm_cb_kpm)
static __thread const char* current_cb_gnb_label = "unknown";

// Funzioni per decodifica RRC e estrazione metriche radio via RC SM
static void log_meas_report_radio_metrics(const NR_MeasResults_t *results, ue_metrics_t *metrics)
{
    for (int i = 0; i < results->measResultServingMOList.list.count; i++) {
        NR_MeasResultServMO_t *measresultservmo = results->measResultServingMOList.list.array[i];
        NR_MeasResultNR_t *measresultnr = &measresultservmo->measResultServingCell;
        NR_MeasQuantityResults_t *mqr = measresultnr->measResult.cellResults.resultsSSB_Cell;

        if (mqr != NULL) {
            // Conversione corretta (3GPP TS 38.133), con null-check per campi OPTIONAL
            const double rrsrp = mqr->rsrp ? (double)(*mqr->rsrp - 156) : -999.0;
            const double rrsrq = mqr->rsrq ? (double)(*mqr->rsrq - 87) / 2.0 : -999.0;
            const double rsinr = mqr->sinr ? (double)(*mqr->sinr - 46) / 2.0 : -999.0;

            printf("[xApp HO] UE %lu - RC Radio Metrics RAW: RSRP=%ld RSRQ=%ld SINR=%ld\n",
                   metrics->ue_id,
                   mqr->rsrp ? *mqr->rsrp : -1,
                   mqr->rsrq ? *mqr->rsrq : -1,
                   mqr->sinr ? *mqr->sinr : -1);

            // Aggiorna le metriche nell'array globale
            if (mqr->rsrp) metrics->rsrp = rrsrp;
            if (mqr->rsrq) metrics->rsrq = rrsrq;
            if (mqr->sinr) metrics->sinr = rsinr;

            printf("[xApp HO] UE %lu - RC Radio Metrics: RSRP %.1f dBm, RSRQ %.1f dB, SINR %.1f dB\n",
                   metrics->ue_id, rrsrp, rrsrq, rsinr);
            if (mqr->rsrp) log_triggering_metric("RSRP", rrsrp);
            if (mqr->rsrq) log_triggering_metric("RSRQ", rrsrq);
            if (mqr->sinr) log_triggering_metric("SINR", rsinr);
        } else {
            printf("[xApp HO] UE %lu - RC Radio Metrics: empty (no SSB cell results)\n", metrics->ue_id);
        }
    }
}

static void log_octet_str_ran_param_value_radio(const e2sm_rc_ind_hdr_frmt_1_t *hdr, byte_array_t octet_str, uint32_t id, ue_metrics_t *metrics)
{
    switch (id) {
        case E2SM_RC_RS1_RRC_MESSAGE:
            if (*hdr->ev_trigger_id == 2 || *hdr->ev_trigger_id == 3 || *hdr->ev_trigger_id == 4) {
                // Decodifica messaggio UL-DCCH (measurementReport, securityModeComplete, rrcSetupComplete)
                NR_UL_DCCH_Message_t *msg = NULL;
                asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_UL_DCCH_Message,
                                                  (void **)&msg, octet_str.buf, octet_str.len, 0, 0);
                if (dec_rval.code == RC_OK) {
                    if (msg->message.present == NR_UL_DCCH_MessageType_PR_c1) {
                        if (msg->message.choice.c1->present == NR_UL_DCCH_MessageType__c1_PR_measurementReport) {
                            NR_MeasResults_t *results = &msg->message.choice.c1->choice.measurementReport->criticalExtensions.choice.measurementReport->measResults;
                            if (results != NULL) {
                                log_meas_report_radio_metrics(results, metrics);
                            }
                        }
                    }
                    ASN_STRUCT_FREE(asn_DEF_NR_UL_DCCH_Message, msg);
                } else {
                    printf("[xApp HO] WARNING: Decodifica UL-DCCH fallita (ev_trigger_id=%d)\n", *hdr->ev_trigger_id);
                }
            }
            break;
        default:
            break;
    }
}

static void log_element_ran_param_value_radio(const e2sm_rc_ind_hdr_frmt_1_t *hdr, ran_parameter_value_t* param_value, uint32_t id, ue_metrics_t *metrics)
{
    assert(param_value != NULL);

    switch (param_value->type) {
        case OCTET_STRING_RAN_PARAMETER_VALUE:
            log_octet_str_ran_param_value_radio(hdr, param_value->octet_str_ran, id, metrics);
            break;
        default:
            break;
    }
}

// Funzioni di utilità
static uint64_t hash_ue_id(uint64_t amf_ue_ngap_id)
{
    return amf_ue_ngap_id % MAX_UE_COUNT;
}

static ue_metrics_t *get_ue_metrics(uint64_t amf_ue_ngap_id)
{
    uint64_t idx = hash_ue_id(amf_ue_ngap_id);
    return &ue_metrics_array[idx];
}

static void init_ue_metrics(uint64_t amf_ue_ngap_id)
{
    ue_metrics_t *metrics = get_ue_metrics(amf_ue_ngap_id);
    metrics->ue_id = amf_ue_ngap_id;
    metrics->valid = false;
    metrics->throughput_dl = 0.0;
    metrics->throughput_ul = 0.0;
    metrics->packet_loss_rate = 0.0;
    metrics->pdcp_delay = 0.0;
    metrics->prb_tot_dl = 0.0;
    metrics->prb_tot_ul = 0.0;
    metrics->pdcp_volume_dl = 0.0;
    metrics->pdcp_volume_ul = 0.0;
    metrics->rlc_sdu_delay_dl = 0.0;
    metrics->rsrp = 0.0;
    metrics->rsrq = 0.0;
    metrics->sinr = 0.0;
    metrics->last_update = 0;
    metrics->handover_triggered = false;
    metrics->last_handover_time = 0;
}


// Logica di decisione handover
static bool should_trigger_handover(ue_metrics_t *metrics)
{
    // Controllo che siano passati almeno 10 secondi dall'avvio della xApp
    int64_t current_time = time_now_us();
    int64_t elapsed_seconds = (current_time - xapp_start_time) / 1000000; // Converti microsecondi in secondi

    if (elapsed_seconds < 5) {
        printf("[HO Decision] UE %lu - Handover bloccato: sono passati solo %ld secondi dall'avvio della xApp (minimo 10)\n",
               metrics->ue_id, elapsed_seconds);
        return false;
    }

    bool trigger_ho = false;

    if ((metrics->throughput_dl + metrics->throughput_ul) < thresholds.throughput_threshold)
    {
        //printf("[HO Decision] UE %lu - Throughput below threshold (%.1f < %.1f kbps)\n",
        //       metrics->ue_id, metrics->throughput_dl + metrics->throughput_ul, thresholds.throughput_threshold);
        trigger_ho = true;
    }

    // if (( metrics->pdcp_delay > thresholds.delay_threshold))
    // {
    //     //printf("[HO Decision] UE %lu - Packet Loss Rate above threshold (%.3f > %.3f)\n",
    //     //       metrics->ue_id, metrics->pdcp_delay, thresholds.delay_threshold);
    //     trigger_ho = true;
    // }
    return trigger_ho;
}

// Funzione per generare subscription KPM valida (presa da xapp_ho_rsrp_rsrq.c)
static int period_ms = 100; // Periodo di reporting in ms

// Funzione di supporto per matching condition
static test_info_lst_t filter_predicate(test_cond_type_e type, test_cond_e cond, int value)
{
    (void)value; // Evita warning per parametro non utilizzato
    test_info_lst_t dst = {0};
    dst.test_cond_type = type;
    dst.S_NSSAI = TRUE_TEST_COND_TYPE;
    dst.test_cond = calloc(1, sizeof(test_cond_e));
    assert(dst.test_cond != NULL && "Memory exhausted");
    *dst.test_cond = cond;
    dst.test_cond_value = calloc(1, sizeof(test_cond_value_t));
    assert(dst.test_cond_value != NULL && "Memory exhausted");
    dst.test_cond_value->type = OCTET_STRING_TEST_COND_VALUE;
    dst.test_cond_value->octet_string_value = calloc(1, sizeof(byte_array_t));
    assert(dst.test_cond_value->octet_string_value != NULL && "Memory exhausted");
    
    // Configurazione corretta per S-NSSAI: SST=1, SD=1
    // Formato: [SST (1 byte)] [SD (3 bytes)]
    const size_t len_nssai = 4;
    dst.test_cond_value->octet_string_value->len = len_nssai;
    dst.test_cond_value->octet_string_value->buf = calloc(len_nssai, sizeof(uint8_t));
    assert(dst.test_cond_value->octet_string_value->buf != NULL && "Memory exhausted");
    
    // SST = 1 (1 byte)
    dst.test_cond_value->octet_string_value->buf[0] = 1;
    // SD = 1 (3 bytes, big-endian)
    dst.test_cond_value->octet_string_value->buf[1] = 0x00;
    dst.test_cond_value->octet_string_value->buf[2] = 0x00;
    dst.test_cond_value->octet_string_value->buf[3] = 0x01;
    
    return dst;
}

static kpm_sub_data_t gen_kpm_subs(kpm_ran_function_def_t const *ran_func)
{
    assert(ran_func != NULL);
    assert(ran_func->ric_event_trigger_style_list != NULL);

    kpm_sub_data_t kpm_sub = {0};

    // Event Trigger
    kpm_sub.ev_trg_def.type = FORMAT_1_RIC_EVENT_TRIGGER;
    kpm_sub.ev_trg_def.kpm_ric_event_trigger_format_1.report_period_ms = period_ms;

    // Action Definition FORMAT_4
    kpm_sub.sz_ad = 1;
    kpm_sub.ad = calloc(1, sizeof(kpm_act_def_t));
    assert(kpm_sub.ad != NULL && "Memory exhausted");

    ric_report_style_item_t *report_item = &ran_func->ric_report_style_list[0];
    kpm_sub.ad[0].type = FORMAT_4_ACTION_DEFINITION;
    // Matching condition (dummy, puoi personalizzare)
    kpm_sub.ad[0].frm_4.matching_cond_lst_len = 1;
    kpm_sub.ad[0].frm_4.matching_cond_lst = calloc(1, sizeof(matching_condition_format_4_lst_t));
    kpm_sub.ad[0].frm_4.matching_cond_lst[0].test_info_lst = filter_predicate(S_NSSAI_TEST_COND_TYPE, EQUAL_TEST_COND, 1);
    // Action Definition Format 1 (come esempio)
    size_t sz_std = report_item->meas_info_for_action_lst_len;
    size_t sz = sz_std; // Rimosso: + 3 metriche custom (RSRP, RSRQ, SINR ora gestite via RC)
    kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst_len = sz;
    kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst = calloc(sz, sizeof(meas_info_format_1_lst_t));
    // Copia metriche standard
    for (size_t i = 0; i < sz_std; ++i)
    {
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].meas_type.type = NAME_MEAS_TYPE;
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].meas_type.name = copy_byte_array(report_item->meas_info_for_action_lst[i].name);
        
        // Assicurati che label_info_lst sia sempre impostato correttamente
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].label_info_lst_len = 1;
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].label_info_lst = calloc(1, sizeof(label_info_lst_t));
        assert(kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].label_info_lst != NULL && "Memory exhausted");
        kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].label_info_lst[0].noLabel = calloc(1, sizeof(enum_value_e));
        assert(kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].label_info_lst[0].noLabel != NULL && "Memory exhausted");
        *kpm_sub.ad[0].frm_4.action_def_format_1.meas_info_lst[i].label_info_lst[0].noLabel = TRUE_ENUM_VALUE;
    }
    // Rimosso: aggiunta metriche custom RSRP, RSRQ, SINR (ora gestite via RC)
    kpm_sub.ad[0].frm_4.action_def_format_1.gran_period_ms = period_ms;
    kpm_sub.ad[0].frm_4.action_def_format_1.cell_global_id = NULL;

    return kpm_sub;
}

// Dichiarazioni anticipate delle funzioni
static rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const* ctrl_def, uint64_t amf_ue_ngap_id);
static void send_handover_control_command(uint64_t ue_id);

// Dichiarazioni delle funzioni
static void configure_target_gnb_dynamic(seq_ran_param_t* nr_cgi_leaf_param);

// Wrapper callback per KPM: impostano current_cb_gnb_label prima di delegare
static void sm_cb_kpm(sm_ag_if_rd_t const *rd);  // forward declaration
static void sm_cb_kpm_0(sm_ag_if_rd_t const *rd) { current_cb_gnb_label = gnb_labels[0]; sm_cb_kpm(rd); }
static void sm_cb_kpm_1(sm_ag_if_rd_t const *rd) { current_cb_gnb_label = gnb_labels[1]; sm_cb_kpm(rd); }
static void sm_cb_kpm_2(sm_ag_if_rd_t const *rd) { current_cb_gnb_label = gnb_labels[2]; sm_cb_kpm(rd); }

// Array di callback indicizzato per nodo (max MAX_NODES)
typedef void (*kpm_cb_fn_t)(sm_ag_if_rd_t const *);
static kpm_cb_fn_t kpm_node_cbs[MAX_NODES];

// Callback per KPM (copiato da xapp_ho_rsrp_rsrq.c)
static void sm_cb_kpm(sm_ag_if_rd_t const *rd)
{
    assert(rd != NULL);
    // Parsing FORMAT_3 come xapp_ho_rsrp_rsrq
    const kpm_ind_data_t *ind = &rd->ind.kpm.ind;
    const kpm_ind_msg_format_3_t *msg_frm_3 = &ind->msg.frm_3;
    static int counter = 1;
    
    printf("\n[%d] KPM indication (FORMAT_3) received\n", counter++);

    int64_t const now = time_now_us();
    pthread_mutex_lock(&metrics_mutex);
    
    for (size_t i = 0; i < msg_frm_3->ue_meas_report_lst_len; i++)
    {
        // log UE ID (opzionale)
        // ue_id_e2sm_t const ue_id_e2sm = msg_frm_3->meas_report_per_ue[i].ue_meas_report_lst;
        // log_ue_id_e2sm[ue_id_e2sm.type](ue_id_e2sm);
        // log measurements
        const kpm_ind_msg_format_1_t *msg = &msg_frm_3->meas_report_per_ue[i].ind_msg_format_1;
        printf("[xApp HO] UE #%zu: %zu metriche\n", i, msg->meas_info_lst_len);

        // Estrai l'UE ID reale dal messaggio KPM
        uint64_t ue_id = 0;
        if (msg_frm_3->meas_report_per_ue[i].ue_meas_report_lst.type == GNB_UE_ID_E2SM) {
            ue_id = msg_frm_3->meas_report_per_ue[i].ue_meas_report_lst.gnb.amf_ue_ngap_id;
            printf("[xApp HO] DEBUG: UE ID estratto da KPM - AMF_UE_NGAP_ID = %lu\n", ue_id);
        } else {
            // Fallback se non è disponibile l'UE ID
            ue_id = i + 1;
            printf("[xApp HO] WARNING: UE ID non disponibile, uso placeholder %lu\n", ue_id);
        }
        ue_metrics_t *metrics = get_ue_metrics(ue_id);

        if (!metrics->valid)
        {
            init_ue_metrics(ue_id);
        }

        for (size_t j = 0; j < msg->meas_data_lst_len; ++j)
        {
            const meas_data_lst_t *data = &msg->meas_data_lst[j];
            for (size_t k = 0; k < data->meas_record_len && k < msg->meas_info_lst_len; ++k)
            {
                const meas_info_format_1_lst_t *info = &msg->meas_info_lst[k];
                if (info->meas_type.type == NAME_MEAS_TYPE)
                {
                    // Solo metriche con valori REALI (non stimate) - RSRP, RSRQ, SINR ora gestiti via RC
                    if (cmp_str_ba("DRB.UEThpDl", info->meas_type.name) == 0)
                    {
                        metrics->throughput_dl = data->meas_record_lst[k].real_val;
                        printf("[xApp HO] UE %lu - DL Throughput = %.1f kbps\n", metrics->ue_id, metrics->throughput_dl);
                        log_triggering_metric("DRB.UEThpDl", metrics->throughput_dl);
                    }
                    else if (cmp_str_ba("DRB.UEThpUl", info->meas_type.name) == 0)
                    {
                        metrics->throughput_ul = data->meas_record_lst[k].real_val;
                        printf("[xApp HO] UE %lu - UL Throughput = %.1f kbps\n", metrics->ue_id, metrics->throughput_ul);
                        log_triggering_metric("DRB.UEThpUl", metrics->throughput_ul);
                    }
                    else if (cmp_str_ba("DRB.PdcpSduDelayDl", info->meas_type.name) == 0)
                    {
                        metrics->pdcp_delay = data->meas_record_lst[k].real_val;
                        printf("[xApp HO] UE %lu - PDCP Delay = %.1f ms\n", metrics->ue_id, metrics->pdcp_delay);
                        log_triggering_metric("DRB.PdcpSduDelayDl", metrics->pdcp_delay);
                    }
                    else if (cmp_str_ba("RRU.PrbTotDl", info->meas_type.name) == 0)
                    {
                        metrics->prb_tot_dl = data->meas_record_lst[k].int_val;
                        printf("[xApp HO] UE %lu - PRB Tot DL = %d\n", metrics->ue_id, (int)metrics->prb_tot_dl);
                        log_triggering_metric("RRU.PrbTotDl", metrics->prb_tot_dl);
                    }
                    else if (cmp_str_ba("RRU.PrbTotUl", info->meas_type.name) == 0)
                    {
                        metrics->prb_tot_ul = data->meas_record_lst[k].int_val;
                        printf("[xApp HO] UE %lu - PRB Tot UL = %d\n", metrics->ue_id, (int)metrics->prb_tot_ul);
                        log_triggering_metric("RRU.PrbTotUl", metrics->prb_tot_ul);
                    }
                    else if (cmp_str_ba("DRB.PdcpSduVolumeDl", info->meas_type.name) == 0)
                    {
                        // Conversione da byte a kbit
                        metrics->pdcp_volume_dl = data->meas_record_lst[k].int_val * 0.008;
                        printf("[xApp HO] UE %lu - PDCP Volume DL = %.3f kbit\n", metrics->ue_id, metrics->pdcp_volume_dl);
                        log_triggering_metric("DRB.PdcpSduVolumeDl", metrics->pdcp_volume_dl);
                    }
                    else if (cmp_str_ba("DRB.PdcpSduVolumeUl", info->meas_type.name) == 0)
                    {
                        // Conversione da byte a kbit
                        metrics->pdcp_volume_ul = data->meas_record_lst[k].int_val * 0.008;
                        printf("[xApp HO] UE %lu - PDCP Volume UL = %.3f kbit\n", metrics->ue_id, metrics->pdcp_volume_ul);
                        log_triggering_metric("DRB.PdcpSduVolumeUl", metrics->pdcp_volume_ul);
                    }
                    else if (cmp_str_ba("DRB.PdcpSduVolumeUL", info->meas_type.name) == 0)
                    {
                        // Conversione da byte a kbit
                        metrics->pdcp_volume_ul = data->meas_record_lst[k].int_val * 0.008;
                        printf("[xApp HO] UE %lu - PDCP Volume UL = %.3f kbit\n", metrics->ue_id, metrics->pdcp_volume_ul);
                        log_triggering_metric("DRB.PdcpSduVolumeUL", metrics->pdcp_volume_ul);
                    }
                    else if (cmp_str_ba("DRB.RlcSduDelayDl", info->meas_type.name) == 0)
                    {
                        metrics->rlc_sdu_delay_dl = data->meas_record_lst[k].real_val;
                        printf("[xApp HO] UE %lu - RLC SDU Delay DL = %.1f\n", metrics->ue_id, metrics->rlc_sdu_delay_dl);
                        log_triggering_metric("DRB.RlcSduDelayDl", metrics->rlc_sdu_delay_dl);
                    }
                    else 
                    //log che il parametro non è supportato
                    {
                        printf("[xApp HO] UE %lu - Parametro non supportato: %.*s\n", metrics->ue_id, (int)info->meas_type.name.len, info->meas_type.name.buf);
                    }

                }
            }
        }
        metrics->last_update = now;
        metrics->valid = true;

        // Debug: stampa lo stato corrente dell'UE
        printf("[HO Decision] UE %lu - Throughput DL=%.1f, UL=%.1f, Delay=%.1f, HO_triggered=%s\n", 
               metrics->ue_id, metrics->throughput_dl, metrics->throughput_ul, metrics->pdcp_delay,
               metrics->handover_triggered ? "YES" : "NO");
        
        // Traccia il primo UE valido per il timer HO
        if (timer_ho_ue_id == 0 && metrics->valid) {
            timer_ho_ue_id = metrics->ue_id;
        }

        // Check timer HO forzato: se --ho-timer N è impostato, triggera HO dopo N secondi
        if (ho_timer_seconds > 0 && !timer_ho_triggered && metrics->ue_id == timer_ho_ue_id) {
            int64_t elapsed_s = (time_now_us() - xapp_start_time) / 1000000LL;
            if (elapsed_s >= ho_timer_seconds) {
                pthread_mutex_lock(&ho_sent_mutex);
                if (!global_handover_sent) {
                    global_handover_sent = true;
                    timer_ho_triggered = true;
                    pthread_mutex_unlock(&ho_sent_mutex);
                    printf("[HO Timer] %llds elapsed (threshold=%ds) - FORCING HANDOVER for UE %lu\n",
                           (long long)elapsed_s, ho_timer_seconds, metrics->ue_id);
                    metrics->handover_triggered = true;
                    metrics->last_handover_time = now;
                    send_handover_control_command(metrics->ue_id);
                } else {
                    timer_ho_triggered = true;
                    pthread_mutex_unlock(&ho_sent_mutex);
                }
            }
        }

        // Valuta se avviare handover (una sola volta per tutta l'esecuzione della xApp)
        // Se --ho-timer è impostato, la logica normale è disabilitata: solo il timer conta
        if (ho_timer_seconds <= 0 && should_trigger_handover(metrics))
        {
            pthread_mutex_lock(&ho_sent_mutex);
            bool should_send = !global_handover_sent;
            if (should_send) {
                global_handover_sent = true;
            }
            pthread_mutex_unlock(&ho_sent_mutex);

            if (should_send) {
                printf("[HO Decision] UE %lu - TRIGGERING HANDOVER!\n", metrics->ue_id);
                metrics->handover_triggered = true;
                metrics->last_handover_time = now;

                send_handover_control_command(metrics->ue_id);
                printf("[HO Decision] HANDOVER COMMAND SENT - Nessun altro comando HO verrà inviato durante questa esecuzione\\n");
            } else {
                printf("[HO Decision] UE %lu - Condizioni per HO rilevate, ma un comando HO è già stato inviato. Nessuna azione.\\n", metrics->ue_id);
            }
        }
    }
    
    pthread_mutex_unlock(&metrics_mutex);
}

// Funzione per generare il messaggio RC di controllo per handover
static rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const* ctrl_def, uint64_t amf_ue_ngap_id) {
    assert(ctrl_def != NULL);
    
    rc_ctrl_req_data_t rc_ctrl = {0};
    
    printf("[RC Control] DEBUG: Numero totale di control styles: %zu\n", ctrl_def->sz_seq_ctrl_style);
    
    // Cerca lo stile "Connected Mode Mobility Control" (style_type = 3)
    for (size_t i = 0; i < ctrl_def->sz_seq_ctrl_style; i++) {
        // Verifica se è lo stile "Connected Mode Mobility Control"
        if (cmp_str_ba("Connected Mode Mobility Control", ctrl_def->seq_ctrl_style[i].name) == 0) {
            //printf("[RC Control] Trovato stile Connected Mode Mobility Control\n");
            // Cerca l'action "Handover Control" (id = 1)
            for (size_t j = 0; j < ctrl_def->seq_ctrl_style[i].sz_seq_ctrl_act; j++) {
                if (cmp_str_ba("Handover Control", ctrl_def->seq_ctrl_style[i].seq_ctrl_act[j].name) == 0) {
                    //printf("[RC Control] Trovato action Handover Control\n");
                    
                    // Imposta il formato dell'header
                    rc_ctrl.hdr.format = ctrl_def->seq_ctrl_style[i].hdr;
                    assert(rc_ctrl.hdr.format == FORMAT_1_E2SM_RC_CTRL_HDR && "Header Format non valido");
                    
                    // Imposta i parametri dell'header
                    rc_ctrl.hdr.frmt_1.ric_style_type = 3; // Connected Mode Mobility Control
                    rc_ctrl.hdr.frmt_1.ctrl_act_id = 1; // Handover Control
                    
                    // VALORIZZAZIONE CORRETTA DELL'UE ID
                    rc_ctrl.hdr.frmt_1.ue_id.type = GNB_UE_ID_E2SM;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.amf_ue_ngap_id = amf_ue_ngap_id;
                    // Set GUAMI PLMN ID as byte array
                    // set_plmn_id(&rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.plmn_id, 1, 0, 1); // Use your actual MCC, MNC_high, MNC_low
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_region_id = 0;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_set_id = 0;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_ptr = 0;
                    
                    //printf("[RC Control] DEBUG: UE ID configurato - AMF_UE_NGAP_ID = %lu\n", amf_ue_ngap_id);
                    
                    // Imposta il formato del messaggio
                    rc_ctrl.msg.format = ctrl_def->seq_ctrl_style[i].msg;
                    assert(rc_ctrl.msg.format == FORMAT_1_E2SM_RC_CTRL_MSG && "Message Format non valido");
                    
                    // Crea parametri RAN per handover
                    rc_ctrl.msg.frmt_1.sz_ran_param = 1; // Target Primary Cell ID
                    rc_ctrl.msg.frmt_1.ran_param = calloc(1, sizeof(seq_ran_param_t));
                    assert(rc_ctrl.msg.frmt_1.ran_param != NULL && "Memory exhausted");
                    
                    // Parametro 1: Target Primary Cell ID
                    seq_ran_param_t* target_primary_cell_param = &rc_ctrl.msg.frmt_1.ran_param[0];
                    target_primary_cell_param->ran_param_id = 1; // Target Primary Cell ID
                    target_primary_cell_param->ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
                    target_primary_cell_param->ran_param_val.strct = calloc(1, sizeof(ran_param_struct_t));
                    assert(target_primary_cell_param->ran_param_val.strct != NULL && "Memory exhausted");
                    ran_param_struct_t* target_primary_cell_struct = target_primary_cell_param->ran_param_val.strct;
                    target_primary_cell_struct->sz_ran_param_struct = 1; //Target Cell
                    target_primary_cell_struct->ran_param_struct = calloc(1, sizeof(seq_ran_param_t));
                    assert(target_primary_cell_struct->ran_param_struct != NULL && "Memory exhausted");

                    // Parametro 2: Target Cell (id=2, STRUCTURE)
                    seq_ran_param_t* target_cell_param = &target_primary_cell_struct->ran_param_struct[0];
                    target_cell_param->ran_param_id = 2;
                    target_cell_param->ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
                    target_cell_param->ran_param_val.strct = calloc(1, sizeof(ran_param_struct_t));
                    assert(target_cell_param->ran_param_val.strct != NULL && "Memory exhausted");
                    ran_param_struct_t* target_cell_struct = target_cell_param->ran_param_val.strct;
                    target_cell_struct->sz_ran_param_struct = 1; // NR Cell
                    target_cell_struct->ran_param_struct = calloc(1, sizeof(seq_ran_param_t));
                    assert(target_cell_struct->ran_param_struct != NULL && "Memory exhausted");

                    // Parametro 3: NR Cell (id=3, STRUCTURE)
                    seq_ran_param_t* nr_cell_param = &target_cell_struct->ran_param_struct[0];
                    nr_cell_param->ran_param_id = 3;
                    nr_cell_param->ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
                    nr_cell_param->ran_param_val.strct = calloc(1, sizeof(ran_param_struct_t));
                    assert(nr_cell_param->ran_param_val.strct != NULL && "Memory exhausted");
                    ran_param_struct_t* nr_cell_struct = nr_cell_param->ran_param_val.strct;
                    nr_cell_struct->sz_ran_param_struct = 1; // NR CGI
                    nr_cell_struct->ran_param_struct = calloc(1, sizeof(seq_ran_param_t));
                    assert(nr_cell_struct->ran_param_struct != NULL && "Memory exhausted");

                    // Parametro 4: NR CGI (id=4, valore foglia)
                    seq_ran_param_t* nr_cgi_leaf_param = &nr_cell_struct->ran_param_struct[0];
                    nr_cgi_leaf_param->ran_param_id = 4;
                    nr_cgi_leaf_param->ran_param_val.type = ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE;
                    nr_cgi_leaf_param->ran_param_val.flag_true = calloc(1, sizeof(ran_parameter_value_t));
                    assert(nr_cgi_leaf_param->ran_param_val.flag_true != NULL && "Memory exhausted");
                    nr_cgi_leaf_param->ran_param_val.flag_true->type = OCTET_STRING_RAN_PARAMETER_VALUE;
                    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.len = 15;
                    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf = calloc(1, 15);
                    assert(nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf != NULL && "Memory exhausted");
                    
                    // Configurazione dinamica basata su metriche
                    // Per ora passiamo NULL come metrics, ma in futuro può essere usato per selezione intelligente
                    configure_target_gnb_dynamic(nr_cgi_leaf_param);
                    
                    // Validazione finale del messaggio
                    if (rc_ctrl.msg.frmt_1.ran_param == NULL) {
                        printf("[RC Control] ERRORE: ran_param è NULL dopo la configurazione\n");
                        // Cleanup e return con struttura vuota
                        free_rc_ctrl_req_data(&rc_ctrl);
                        rc_ctrl.hdr.frmt_1.ctrl_act_id = 0;
                        return rc_ctrl;
                    }
                    
                    //printf("[RC Control] Messaggio RC di handover generato con successo con parametri completi\n");
                    return rc_ctrl;
                }
            }
            
            // Se arriviamo qui, abbiamo trovato lo stile ma non l'action
            //printf("[RC Control] ERRORE: Stile Connected Mode Mobility Control trovato ma action Handover Control non trovato\n");
            break;
        }
    }

    
    // Inizializza la struttura di ritorno con valori di default per evitare crash
    rc_ctrl.hdr.format = FORMAT_1_E2SM_RC_CTRL_HDR;
    rc_ctrl.hdr.frmt_1.ric_style_type = 0;
    rc_ctrl.hdr.frmt_1.ctrl_act_id = 0; // Questo causerà l'assertion failure ma in modo controllato
    rc_ctrl.msg.format = FORMAT_1_E2SM_RC_CTRL_MSG;
    rc_ctrl.msg.frmt_1.sz_ran_param = 0;
    rc_ctrl.msg.frmt_1.ran_param = NULL;
    
    return rc_ctrl;
}

// Funzione per inviare comando RC di handover a tutti i nodi connessi (solo una volta per UE)
// Il comando viene inviato a tutti i nodi, ma solo il source gNB (quello con l'UE connesso)
// eseguirà effettivamente l'handover. Gli altri nodi ignoreranno il comando.
static void send_handover_control_command(uint64_t ue_id) {
    if (global_nodes == NULL) {
        printf("[RC Control] Errore: nodi globali non disponibili\n");
        return;
    }

    int const RC_ran_function = 3;  // RC service model ID
    int nodes_notified = 0;

    // Invia il comando a tutti i nodi con RC supportato
    for (size_t node_idx = 0; node_idx < global_nodes->len; node_idx++) {
        e2_node_connected_xapp_t* node = &global_nodes->n[node_idx];

        // Cerca l'indice della RAN Function RC
        size_t rf_idx = 0;
        bool found_rc = false;
        for (; rf_idx < node->len_rf; rf_idx++) {
            if (node->rf[rf_idx].id == RC_ran_function) {
                found_rc = true;
                break;
            }
        }

        if (!found_rc) {
            printf("[RC Control] Nodo %zu: RC service model non trovato, saltando\n", node_idx);
            continue;
        }

        // Verifica che il controllo sia supportato
        if (node->rf[rf_idx].defn.rc.ctrl == NULL) {
            printf("[RC Control] Nodo %zu: controllo RC non supportato, saltando\n", node_idx);
            continue;
        }

        printf("[RC Control] Nodo %zu: Invio comando handover per UE %lu\n", node_idx, ue_id);

        // Genera il messaggio RC di controllo
        rc_ctrl_req_data_t rc_ctrl = gen_rc_ctrl_msg(node->rf[rf_idx].defn.rc.ctrl, ue_id);

        // Verifica che l'action ID sia valido prima di inviare
        if (rc_ctrl.hdr.frmt_1.ctrl_act_id <= 0) {
            printf("[RC Control] Nodo %zu: ERRORE - Action ID non valido (%d), impossibile inviare comando handover\n",
                   node_idx, rc_ctrl.hdr.frmt_1.ctrl_act_id);
            continue;
        }

        // Invia il comando al nodo
        control_sm_xapp_api(&node->id, RC_ran_function, &rc_ctrl);
        free_rc_ctrl_req_data(&rc_ctrl);

        printf("[RC Control] Nodo %zu: Comando handover inviato\n", node_idx);
        nodes_notified++;
    }

    if (nodes_notified > 0) {
        printf("[RC Control] Comando handover per UE %lu inviato a %d nodi (solo il source gNB eseguira' l'HO)\n",
               ue_id, nodes_notified);
    } else {
        printf("[RC Control] ERRORE: Nessun nodo con RC supportato trovato\n");
    }
}

// Funzione aggiornata per configurare il target gNB B210bis
static void configure_target_gnb_dynamic(seq_ran_param_t* nr_cgi_leaf_param) {
    // Validazione parametri
    if (nr_cgi_leaf_param == NULL || 
        nr_cgi_leaf_param->ran_param_val.flag_true == NULL ||
        nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf == NULL) {
        printf("[RC Control] ERRORE: Parametri NULL in configure_target_gnb_dynamic\n");
        return;
    }
    // Valori corretti per MCC=1, MNC=1
    // PLMN: MCC=1, MNC=1 (formato diretto come si aspetta l'E2 Agent)
    uint8_t mcc = 1;
    uint8_t mnc_high = 0;  // Prima cifra MNC
    uint8_t mnc_low = 1;   // Seconda cifra MNC
    
    // Codifica diretta come si aspetta l'E2 Agent (non secondo specifiche 3GPP)
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[0] = mcc;        // MCC = 1
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[1] = mnc_high;   // MNC high = 0
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[2] = mnc_low;    // MNC low = 1
    // gNB_ID = 0x002 (4 byte, big-endian) - dal file gnb_b210bis.conf
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[3] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[4] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[5] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[6] = 0x02;
    // CellID = 12345679L (4 byte, big-endian) - dal file gnb_b210bis.conf
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[7]  = (12345679L >> 24) & 0xFF;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[8]  = (12345679L >> 16) & 0xFF;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[9]  = (12345679L >> 8) & 0xFF;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[10] = 12345679L & 0xFF;
    // PhysicalCellId = 1 (2 byte, big-endian) - dal file gnb_b210bis.conf
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[11] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[12] = 0x01;
    // TAC = 0x0001 (2 byte, big-endian) - dal file gnb_b210bis.conf
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[13] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[14] = 0x01;
    // Lunghezza totale - 15 byte per NR CGI completo
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.len = 15;
    
    // Debug: stampa i valori configurati
    printf("[RC Control] DEBUG: NR CGI configurato - MCC=%d, MNC=%d%d, gNB_ID=0x%02x, CellID=%lu, TAC=0x%04x\n",
           mcc, mnc_high, mnc_low,
           nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[6],
           12345679L,
           (nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[13] << 8) | 
           nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[14]);
    
    // Debug: stampa l'intero octet string
    printf("[RC Control] DEBUG: Octet string completo (len=%zu): ", 
           nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.len);
    for (int i = 0; i < 15; i++) {
        printf("%02x ", nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[i]);
    }
    printf("\n");
    
    // Debug: verifica manuale della decodifica
    uint8_t test_buf[15] = {0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0xBC, 0x61, 0x4F, 0x00, 0x01, 0x00, 0x01};
    printf("[RC Control] DEBUG: Test decodifica NR CGI - MCC=%d, MNC=%d%d, gNB_ID=0x%02x, CellID=%lu\n", 
           test_buf[0], test_buf[1], test_buf[2], test_buf[6], 
           ((uint64_t)test_buf[7] << 24) | ((uint64_t)test_buf[8] << 16) | ((uint64_t)test_buf[9] << 8) | test_buf[10]);
}

// Funzione per inizializzare il file CSV
static void init_csv_logging(void) {
    // Inizializza il mutex per il file CSV
    pthread_mutexattr_t csv_attr = {0};
    int rc = pthread_mutex_init(&csv_mutex, &csv_attr);
    assert(rc == 0);

    // Assicura load_label valido per il filename
    for (size_t k = 0; k < strlen(load_label); k++) {
        if (load_label[k] == ' ') load_label[k] = '_';
    }

    // Create output directory if it doesn't exist
    struct stat st = {0};
    if (stat(csv_output_dir, &st) == -1) {
        mkdir(csv_output_dir, 0755);
    }

    // Genera nome file con timestamp YYYYMMDD_HHMMSS_mmm
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_info = localtime(&now);
    char basename[256];
    int ms = tv.tv_usec / 1000;
    strftime(basename, sizeof(basename), "ho_timer_%Y%m%d_%H%M%S", tm_info);
    // Append milliseconds and .csv extension
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/%s_%03d.csv", csv_output_dir, basename, ms);

    // Apri il file CSV in modalità write (nuovo file per ogni esecuzione)
    csv_file = fopen(filename, "w");
    if (csv_file == NULL) {
        printf("[CSV Logging] ERRORE: Impossibile aprire il file CSV %s per il logging\n", filename);
        return;
    }
    
    // Scrivi commento descrittivo del test (se presente)
    if (csv_comment[0] != '\0') {
        fprintf(csv_file, "# %s\n", csv_comment);
    }
    // Scrivi sempre l'header per il nuovo file
    fprintf(csv_file, "timestamp,gnb,metric_name,metric_value\n");
    fflush(csv_file);
    printf("[CSV Logging] File CSV %s inizializzato con header (label=%s, comment='%s', ho_timer=%ds)\n",
           filename, load_label, csv_comment, ho_timer_seconds);
}

// Funzione per loggare una metrica che ha triggerato l'handover
static void log_triggering_metric(const char* metric_name, double metric_value) {
    if (csv_file == NULL) {
        return;
    }
    
    pthread_mutex_lock(&csv_mutex);
    
    // Ottieni timestamp corrente con millisecondi
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    sprintf(timestamp + strlen(timestamp), ".%03ld", tv.tv_usec / 1000);
    
    // Scrivi la riga CSV con label gNB
    fprintf(csv_file, "%s,%s,%s,%.3f\n",
            timestamp, current_cb_gnb_label, metric_name, metric_value);
    fflush(csv_file);
    
    pthread_mutex_unlock(&csv_mutex);
}

// Funzione per chiudere il file CSV
static void close_csv_logging(void) {
    if (csv_file != NULL) {
        pthread_mutex_lock(&csv_mutex);
        fclose(csv_file);
        csv_file = NULL;
        pthread_mutex_unlock(&csv_mutex);
        
        pthread_mutex_destroy(&csv_mutex);
        printf("[CSV Logging] File CSV chiuso\n");
    }
}

// -------------------------------------------------------
// RC Report Service Style 1: Message Copy (RSRP/RSRQ/SINR)
// -------------------------------------------------------

// Callback RC: riceve indicazioni RC (Format 1 Message Copy) con metriche radio RSRP/RSRQ/SINR
// Nota: la UE ID viene estratta dai RAN parameters (E2SM_RC_RS1_UE_ID), non dall'header
static void sm_cb_rc(sm_ag_if_rd_t const *rd)
{
    assert(rd != NULL);
    assert(rd->type == INDICATION_MSG_AGENT_IF_ANS_V0);

    static int rc_counter = 1;
    printf("\n[%d] RC Indication received\n", rc_counter++);

    const e2sm_rc_ind_hdr_format_e hdr_type = rd->ind.rc.ind.hdr.format;
    const e2sm_rc_ind_msg_format_e msg_type = rd->ind.rc.ind.msg.format;

    if (hdr_type != FORMAT_1_E2SM_RC_IND_HDR || msg_type != FORMAT_1_E2SM_RC_IND_MSG) {
        printf("[RC CB] Formato indication non supportato (hdr=%d, msg=%d)\n", hdr_type, msg_type);
        return;
    }

    const e2sm_rc_ind_hdr_frmt_1_t *hdr = &rd->ind.rc.ind.hdr.frmt_1;
    const e2sm_rc_ind_msg_frmt_1_t *msg = &rd->ind.rc.ind.msg.frmt_1;

    printf("[RC CB] ev_trigger_id=%s, sz_seq_ran_param=%zu\n",
           hdr->ev_trigger_id ? "set" : "null", msg->sz_seq_ran_param);

    pthread_mutex_lock(&metrics_mutex);

    // Trova l'ultimo UE attivo dall'array delle metriche KPM
    // (RC Report Style 1 non include UE ID nel parametro, solo RRC Message)
    uint64_t ue_id = 0;
    for (int idx = 0; idx < MAX_UE_COUNT; idx++) {
        if (ue_metrics_array[idx].valid && ue_metrics_array[idx].ue_id > 0) {
            ue_id = ue_metrics_array[idx].ue_id;
            break;
        }
    }
    if (ue_id == 0) ue_id = 1; // fallback

    ue_metrics_t *metrics = get_ue_metrics(ue_id);
    if (!metrics->valid) init_ue_metrics(ue_id);

    for (size_t j = 0; j < msg->sz_seq_ran_param; j++) {
        seq_ran_param_t *ran_param = &msg->seq_ran_param[j];

        printf("[RC CB] RAN param id=%d per UE %lu\n", ran_param->ran_param_id, ue_id);

        switch (ran_param->ran_param_val.type) {
            case ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE:
                log_element_ran_param_value_radio(hdr, ran_param->ran_param_val.flag_false, ran_param->ran_param_id, metrics);
                break;
            case ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE:
                log_element_ran_param_value_radio(hdr, ran_param->ran_param_val.flag_true, ran_param->ran_param_id, metrics);
                break;
            default:
                break;
        }
    }

    pthread_mutex_unlock(&metrics_mutex);
}

// Genera la subscription RC Report Service Style 1 (Message Copy) per measurementReport
static rc_sub_data_t* gen_rc_sub_report_style1(const ran_func_def_report_t *rc_report)
{
    assert(rc_report != NULL);

    printf("[RC Sub] DEBUG: Cercando Report Service Style 1 tra %zu stili disponibili\n", rc_report->sz_seq_report_sty);

    // Cerca lo stile Report 1 (Message Copy)
    for (size_t i = 0; i < rc_report->sz_seq_report_sty; i++) {
        const seq_report_sty_t *sty = &rc_report->seq_report_sty[i];
        printf("[RC Sub] DEBUG: Stile %zu - report_type=%u, ev_trig_type=%u, name=%.*s\n",
               i, sty->report_type, sty->ev_trig_type, (int)sty->name.len, sty->name.buf);

        if (sty->report_type != 1) // Report Service Style 1
            continue;
        if (sty->ev_trig_type != FORMAT_1_E2SM_RC_EV_TRIGGER_FORMAT)
            continue;

        printf("[RC Sub] DEBUG: Trovato Report Service Style 1\n");

        rc_sub_data_t *rc_sub = calloc(1, sizeof(rc_sub_data_t));
        assert(rc_sub != NULL);

        // Event Trigger Format 1: intercetta measurementReport (UL-DCCH msg id=1)
        rc_sub->et.format = FORMAT_1_E2SM_RC_EV_TRIGGER_FORMAT;
        rc_sub->et.frmt_1.sz_msg_ev_trg = 1;
        rc_sub->et.frmt_1.msg_ev_trg = calloc(1, sizeof(msg_ev_trg_t));
        assert(rc_sub->et.frmt_1.msg_ev_trg != NULL);

        msg_ev_trg_t *ev = &rc_sub->et.frmt_1.msg_ev_trg[0];
        memset(ev, 0, sizeof(*ev));
        ev->ev_trigger_cond_id  = 2; // condition ID 2 = UL-DCCH measurementReport
        ev->msg_type            = RRC_MSG_MSG_TYPE_EV_TRG;
        ev->rrc_msg.type        = NR_RRC_MESSAGE_ID;
        ev->rrc_msg.nr          = UL_DCCH_NR_RRC_CLASS;
        ev->rrc_msg.rrc_msg_id  = 1; // measurementReport

        printf("[RC Sub] DEBUG: Event trigger configurato - ev_trigger_cond_id=2, msg_type=RRC, class=UL_DCCH, msg_id=1\n");

        // Action Definition Format 1: riporta E2SM_RC_RS1_RRC_MESSAGE (id=3)
        rc_sub->sz_ad = 1;
        rc_sub->ad = calloc(1, sizeof(e2sm_rc_action_def_t));
        assert(rc_sub->ad != NULL);
        rc_sub->ad[0].ric_style_type = 1;
        rc_sub->ad[0].format = FORMAT_1_E2SM_RC_ACT_DEF;
        rc_sub->ad[0].frmt_1.sz_param_report_def = 1;
        rc_sub->ad[0].frmt_1.param_report_def = calloc(1, sizeof(param_report_def_t));
        assert(rc_sub->ad[0].frmt_1.param_report_def != NULL);
        rc_sub->ad[0].frmt_1.param_report_def[0].ran_param_id  = E2SM_RC_RS1_RRC_MESSAGE;
        rc_sub->ad[0].frmt_1.param_report_def[0].ran_param_def = NULL;

        printf("[RC Sub] DEBUG: Action Definition configurata - ric_style_type=1, ran_param_id=%d (RRC_MESSAGE)\n",
               E2SM_RC_RS1_RRC_MESSAGE);

        return rc_sub;
    }

    printf("[RC Sub] AVVISO: Report Service Style 1 (Message Copy) non trovato nelle capabilities del nodo\n");
    return NULL;
}

int main(int argc, char *argv[])
{
    // Termina l'xApp dopo 30 secondi tramite SIGALRM (alarm avviato dopo le subscription)
    setenv("XAPP_DURATION", "30", 1);  // 30 secondi
    signal(SIGALRM, sigalrm_handler);

    // Parse argomenti custom PRIMA di init_fr_args, poi rimuovili da argv
    // Uso: ./xapp_advanced_handover_simple [--ho-timer N] [--label NOME] [--comment "TESTO"] [args FlexRIC...]
    int new_argc = 1;
    char *new_argv[argc];
    new_argv[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ho-timer") == 0 && i + 1 < argc) {
            ho_timer_seconds = atoi(argv[i + 1]);
            printf("[Config] HO timer forzato: %d secondi\n", ho_timer_seconds);
            i++; // salta il valore
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            strncpy(load_label, argv[i + 1], sizeof(load_label) - 1);
            load_label[sizeof(load_label) - 1] = '\0';
            printf("[Config] Load label: %s\n", load_label);
            i++; // salta il valore
        } else if (strcmp(argv[i], "--comment") == 0 && i + 1 < argc) {
            strncpy(csv_comment, argv[i + 1], sizeof(csv_comment) - 1);
            csv_comment[sizeof(csv_comment) - 1] = '\0';
            printf("[Config] CSV comment: %s\n", csv_comment);
            i++; // salta il valore
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            strncpy(csv_output_dir, argv[i + 1], sizeof(csv_output_dir) - 1);
            csv_output_dir[sizeof(csv_output_dir) - 1] = '\0';
            printf("[Config] CSV output dir: %s\n", csv_output_dir);
            i++; // salta il valore
        } else {
            new_argv[new_argc++] = argv[i];
        }
    }

    // Inizializza labels gNB di default
    for (int i = 0; i < MAX_NODES; i++) {
        snprintf(gnb_labels[i], sizeof(gnb_labels[i]), "gnb%d", i);
    }
    // Inizializza array callback per nodo
    kpm_node_cbs[0] = sm_cb_kpm_0;
    kpm_node_cbs[1] = sm_cb_kpm_1;
    kpm_node_cbs[2] = sm_cb_kpm_2;
    for (int i = 3; i < MAX_NODES; i++) kpm_node_cbs[i] = sm_cb_kpm_0; // fallback

    fr_args_t args = init_fr_args(new_argc, new_argv);

    // Init the xApp
    init_xapp_api(&args);
    sleep(1);

    // Inizializza il tempo di avvio della xApp
    xapp_start_time = time_now_us();
    printf("[Advanced Handover Simple]: xApp avviata al tempo %ld\n", xapp_start_time);

    e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
    assert(nodes.len > 0);

    // Inizializza la variabile globale per i nodi
    global_nodes = &nodes;

    printf("[Advanced Handover Simple]: Connected E2 nodes = %d\n", nodes.len);
 
    // Init mutex
    pthread_mutexattr_t attr = {0};
    int rc = pthread_mutex_init(&metrics_mutex, &attr);
    assert(rc == 0);

    // Init UE metrics array
    memset(ue_metrics_array, 0, sizeof(ue_metrics_array));

    // Inizializza il file CSV
    init_csv_logging();

    sm_ans_xapp_t *kpm_handles = calloc(nodes.len, sizeof(sm_ans_xapp_t));
    assert(kpm_handles != NULL);

    // Subscribe to KPM
    int const KPM_ran_function = 2;

    
    for (size_t i = 0; i < nodes.len; ++i)
    {
        e2_node_connected_xapp_t *n = &nodes.n[i];

        // Cerca il service model KPM
        size_t idx = 0;
        bool found_kpm = false;
        for (; idx < n->len_rf; idx++)
        {
            if (n->rf[idx].id == KPM_ran_function)
            {
                found_kpm = true;
                break;
            }
        }

        if (found_kpm && n->rf[idx].defn.kpm.ric_report_style_list != NULL)
        {
            // Determina label gNB: source=0x12345, target=0x002
            uint32_t nb_id = n->id.nb_id.nb_id;
            if (nb_id == 0x12345) {
                snprintf(gnb_labels[i], sizeof(gnb_labels[i]), "source");
            } else if (nb_id == 0x002) {
                snprintf(gnb_labels[i], sizeof(gnb_labels[i]), "target");
            } else {
                snprintf(gnb_labels[i], sizeof(gnb_labels[i]), "gnb%zu_0x%x", i, nb_id);
            }
            printf("[xApp HO] Sottoscrizione KPM per nodo %zu (nb_id=0x%x, label='%s')\n",
                   i, nb_id, gnb_labels[i]);

            // Usa callback specifica per nodo (imposta current_cb_gnb_label thread-local)
            size_t cb_idx = (i < MAX_NODES) ? i : 0;
            kpm_sub_data_t kpm_sub = gen_kpm_subs(&n->rf[idx].defn.kpm);
            kpm_handles[i] = report_sm_xapp_api(&n->id, KPM_ran_function, &kpm_sub, kpm_node_cbs[cb_idx]);
            assert(kpm_handles[i].success == true);
            free_kpm_sub_data(&kpm_sub);
        }
    }

    // Subscribe to RC (Report Service Style 1 - Message Copy: RSRP/RSRQ/SINR via RRC MeasurementReport)
    int const RC_ran_function = 3;
    sm_ans_xapp_t *rc_handles = calloc(nodes.len, sizeof(sm_ans_xapp_t));
    assert(rc_handles != NULL);

    printf("[xApp HO] Inizio RC subscription su %d nodi\n", nodes.len);

    for (size_t i = 0; i < nodes.len; ++i)
    {
        e2_node_connected_xapp_t *n = &nodes.n[i];

        // Cerca il service model RC
        size_t rc_idx = 0;
        bool found_rc = false;
        for (; rc_idx < n->len_rf; rc_idx++) {
            if (n->rf[rc_idx].id == RC_ran_function) {
                found_rc = true;
                break;
            }
        }

        if (!found_rc) {
            printf("[xApp HO] Nodo %zu: RC RAN function non trovata (len_rf=%zu)\n", i, n->len_rf);
            continue;
        }

        if (n->rf[rc_idx].defn.rc.report == NULL) {
            printf("[xApp HO] Nodo %zu: RC Report capability è NULL\n", i);
            continue;
        }

        printf("[xApp HO] Nodo %zu: RC Report trovato, generando subscription\n", i);
        rc_sub_data_t *rc_sub = gen_rc_sub_report_style1(n->rf[rc_idx].defn.rc.report);
        if (rc_sub != NULL) {
            printf("[xApp HO] Inviando RC subscription (Report Style 1) al nodo %zu\n", i);
            rc_handles[i] = report_sm_xapp_api(&n->id, RC_ran_function, rc_sub, sm_cb_rc);
            if (rc_handles[i].success) {
                printf("[xApp HO] ✓ RC subscription per nodo %zu confermata\n", i);
            } else {
                printf("[xApp HO] ✗ RC subscription per nodo %zu fallita\n", i);
            }
            free_rc_sub_data(rc_sub);
            free(rc_sub);
        } else {
            printf("[xApp HO] gen_rc_sub_report_style1 ha restituito NULL per nodo %zu\n", i);
        }
    }
    printf("[xApp HO] RC subscription completata\n");

    // Avvia il timer di 30s ora che le subscription sono attive
    alarm(30);
    printf("[xApp HO] Timer 30s avviato\n");

    xapp_wait_end_api();

    // Cleanup
    printf("[Advanced Handover Simple]: Starting cleanup...\n");
    
    for (int i = 0; i < nodes.len; ++i)
    {
        if (kpm_handles[i].success == true) {
            printf("[Cleanup] Removing KPM subscription for node %d\n", i);
            rm_report_sm_xapp_api(kpm_handles[i].u.handle);
        }
        if (rc_handles[i].success == true) {
            printf("[Cleanup] Removing RC subscription for node %d\n", i);
            rm_report_sm_xapp_api(rc_handles[i].u.handle);
        }
    }

    // Chiudi il file CSV
    close_csv_logging();

    printf("[Advanced Handover Simple]: Cleanup completed\n");

    // Reset della variabile globale
    global_nodes = NULL;

    free(kpm_handles);
    free(rc_handles);

    free_e2_node_arr_xapp(&nodes);

    rc = pthread_mutex_destroy(&metrics_mutex);
    assert(rc == 0);

    rc = pthread_mutex_destroy(&ho_sent_mutex);
    assert(rc == 0);

    printf("[Advanced Handover Simple]: Test xApp completed successfully\n");
    return 0;
}