/*
 * xApp per handover avanzato tra due gNB - Versione Timer-Based
 * Setup:
 * - Source: gNB B210 (Core1) - Global ID=0x12345, nr_cellid=12345678L
 * - Target: gNB N310 (Core2) - Global ID=0x002, nr_cellid=12345679L
 *
 * Comportamento handover:
 * - Il comando di handover viene inviato esattamente 7 secondi dopo il primo report KPM
 * - Indipendentemente dalle metriche radio, solo il timer determina il trigger
 * - Il comando viene inviato una sola volta durante tutta l'esecuzione della xApp
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
#include "../../../../src/lib/sm/ie/ue_id.h"
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

#define MAX_UE_COUNT 1000
#define MAX_NODES 10

// Variabili globali per il timer e controllo handover
static int64_t first_kpm_report_time = 0;      // Timestamp del primo report KPM
static bool first_kpm_received = false;         // Flag: primo report ricevuto?
static bool global_handover_sent = false;       // Flag: handover già inviato?
static pthread_mutex_t ho_timer_mutex = PTHREAD_MUTEX_INITIALIZER;  // Protegge le variabili timer

// Array globale per mantenere tutti i nodi connessi
static e2_node_arr_xapp_t* global_nodes = NULL;

// Variabile globale per tracciare il tempo di avvio della xApp
static int64_t xapp_start_time = 0;

// Dichiarazioni anticipate delle funzioni
static rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const* ctrl_def, uint64_t amf_ue_ngap_id);
static void send_handover_control_command(uint64_t ue_id);
static void configure_target_gnb_dynamic(seq_ran_param_t* nr_cgi_leaf_param);

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
    size_t sz = sz_std;
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
    kpm_sub.ad[0].frm_4.action_def_format_1.gran_period_ms = period_ms;
    kpm_sub.ad[0].frm_4.action_def_format_1.cell_global_id = NULL;

    return kpm_sub;
}

// Callback per KPM - LOGICA BASATA SU TIMER (7 SECONDI)
static void sm_cb_kpm(sm_ag_if_rd_t const *rd)
{
    assert(rd != NULL);
    const kpm_ind_data_t *ind = &rd->ind.kpm.ind;
    const kpm_ind_msg_format_3_t *msg_frm_3 = &ind->msg.frm_3;
    static int counter = 1;

    printf("\n[%d] KPM indication (FORMAT_3) received\n", counter++);

    int64_t const now = time_now_us();

    pthread_mutex_lock(&ho_timer_mutex);

    // Primo report KPM ricevuto - avvia il timer
    if (!first_kpm_received) {
        first_kpm_report_time = now;
        first_kpm_received = true;
        printf("[Timer HO] Primo report KPM ricevuto - Timer avviato al tempo %ld\n", first_kpm_report_time);
    }

    // Calcola tempo trascorso dal primo report (in secondi)
    int64_t elapsed_us = now - first_kpm_report_time;
    double elapsed_sec = (double)elapsed_us / 1000000.0;

    // Log della progressione del timer
    if (!global_handover_sent) {
        printf("[Timer HO] Tempo trascorso: %.2f/7.0 secondi\n", elapsed_sec);
    }

    // Controlla se sono passati 7 secondi
    if (elapsed_sec >= 7.0 && !global_handover_sent)
    {
        global_handover_sent = true;

        // Estrai l'UE ID dal primo messaggio disponibile
        uint64_t ue_id = 0;
        if (msg_frm_3->ue_meas_report_lst_len > 0) {
            if (msg_frm_3->meas_report_per_ue[0].ue_meas_report_lst.type == GNB_UE_ID_E2SM) {
                ue_id = msg_frm_3->meas_report_per_ue[0].ue_meas_report_lst.gnb.amf_ue_ngap_id;
            } else {
                ue_id = 1;  // Fallback
            }
        }

        pthread_mutex_unlock(&ho_timer_mutex);

        printf("[Timer HO] === 7 SECONDI TRASCORSI === TRIGGERING HANDOVER per UE %lu ===\n", ue_id);
        send_handover_control_command(ue_id);
        printf("[Timer HO] Comando handover inviato - Nessun altro comando HO verrà inviato\n");

        return;
    }

    pthread_mutex_unlock(&ho_timer_mutex);
}

// Funzione per generare il messaggio RC di controllo per handover
static rc_ctrl_req_data_t gen_rc_ctrl_msg(ran_func_def_ctrl_t const* ctrl_def, uint64_t amf_ue_ngap_id) {
    assert(ctrl_def != NULL);

    rc_ctrl_req_data_t rc_ctrl = {0};

    printf("[RC Control] DEBUG: Numero totale di control styles: %zu\n", ctrl_def->sz_seq_ctrl_style);

    // Cerca lo stile "Connected Mode Mobility Control" (style_type = 3)
    for (size_t i = 0; i < ctrl_def->sz_seq_ctrl_style; i++) {
        if (cmp_str_ba("Connected Mode Mobility Control", ctrl_def->seq_ctrl_style[i].name) == 0) {
            // Cerca l'action "Handover Control" (id = 1)
            for (size_t j = 0; j < ctrl_def->seq_ctrl_style[i].sz_seq_ctrl_act; j++) {
                if (cmp_str_ba("Handover Control", ctrl_def->seq_ctrl_style[i].seq_ctrl_act[j].name) == 0) {

                    // Imposta il formato dell'header
                    rc_ctrl.hdr.format = ctrl_def->seq_ctrl_style[i].hdr;
                    assert(rc_ctrl.hdr.format == FORMAT_1_E2SM_RC_CTRL_HDR && "Header Format non valido");

                    // Imposta i parametri dell'header
                    rc_ctrl.hdr.frmt_1.ric_style_type = 3; // Connected Mode Mobility Control
                    rc_ctrl.hdr.frmt_1.ctrl_act_id = 1; // Handover Control

                    // VALORIZZAZIONE DELL'UE ID
                    rc_ctrl.hdr.frmt_1.ue_id.type = GNB_UE_ID_E2SM;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.amf_ue_ngap_id = amf_ue_ngap_id;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_region_id = 0;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_set_id = 0;
                    rc_ctrl.hdr.frmt_1.ue_id.gnb.guami.amf_ptr = 0;

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

                    // Configurazione dinamica del target gNB
                    configure_target_gnb_dynamic(nr_cgi_leaf_param);

                    // Validazione finale del messaggio
                    if (rc_ctrl.msg.frmt_1.ran_param == NULL) {
                        printf("[RC Control] ERRORE: ran_param è NULL dopo la configurazione\n");
                        free_rc_ctrl_req_data(&rc_ctrl);
                        rc_ctrl.hdr.frmt_1.ctrl_act_id = 0;
                        return rc_ctrl;
                    }

                    return rc_ctrl;
                }
            }
            break;
        }
    }

    // Inizializza la struttura di ritorno con valori di default per evitare crash
    rc_ctrl.hdr.format = FORMAT_1_E2SM_RC_CTRL_HDR;
    rc_ctrl.hdr.frmt_1.ric_style_type = 0;
    rc_ctrl.hdr.frmt_1.ctrl_act_id = 0;
    rc_ctrl.msg.format = FORMAT_1_E2SM_RC_CTRL_MSG;
    rc_ctrl.msg.frmt_1.sz_ran_param = 0;
    rc_ctrl.msg.frmt_1.ran_param = NULL;

    return rc_ctrl;
}

// Funzione per inviare comando RC di handover a tutti i nodi connessi
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
        printf("[RC Control] Comando handover per UE %lu inviato a %d nodi\n", ue_id, nodes_notified);
    } else {
        printf("[RC Control] ERRORE: Nessun nodo con RC supportato trovato\n");
    }
}

// Funzione per configurare il target gNB
static void configure_target_gnb_dynamic(seq_ran_param_t* nr_cgi_leaf_param) {
    if (nr_cgi_leaf_param == NULL ||
        nr_cgi_leaf_param->ran_param_val.flag_true == NULL ||
        nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf == NULL) {
        printf("[RC Control] ERRORE: Parametri NULL in configure_target_gnb_dynamic\n");
        return;
    }
    // Valori corretti per MCC=1, MNC=1
    uint8_t mcc = 1;
    uint8_t mnc_high = 0;  // Prima cifra MNC
    uint8_t mnc_low = 1;   // Seconda cifra MNC

    // Codifica diretta
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[0] = mcc;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[1] = mnc_high;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[2] = mnc_low;
    // gNB_ID = 0x002 (4 byte, big-endian)
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[3] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[4] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[5] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[6] = 0x02;
    // CellID = 12345679L (4 byte, big-endian)
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[7]  = (12345679L >> 24) & 0xFF;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[8]  = (12345679L >> 16) & 0xFF;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[9]  = (12345679L >> 8) & 0xFF;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[10] = 12345679L & 0xFF;
    // PhysicalCellId = 1 (2 byte, big-endian)
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[11] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[12] = 0x01;
    // TAC = 0x0001 (2 byte, big-endian)
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[13] = 0x00;
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[14] = 0x01;
    // Lunghezza totale
    nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.len = 15;

    printf("[RC Control] DEBUG: NR CGI configurato - MCC=%d, MNC=%d%d, gNB_ID=0x%02x, CellID=%lu\n",
           mcc, mnc_high, mnc_low,
           nr_cgi_leaf_param->ran_param_val.flag_true->octet_str_ran.buf[6],
           12345679L);
}

int main(int argc, char *argv[])
{
    // Imposta una durata molto lunga per l'xApp (24 ore)
    setenv("XAPP_DURATION", "86400", 1);

    fr_args_t args = init_fr_args(argc, argv);

    // Init the xApp
    init_xapp_api(&args);
    sleep(1);

    // Inizializza il tempo di avvio della xApp
    xapp_start_time = time_now_us();
    printf("[Timer Handover xApp]: xApp avviata al tempo %ld\n", xapp_start_time);

    e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
    assert(nodes.len > 0);

    // Inizializza la variabile globale per i nodi
    global_nodes = &nodes;

    printf("[Timer Handover xApp]: Connected E2 nodes = %d\n", nodes.len);

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
            printf("[xApp] Sottoscrizione KPM per nodo %zu\n", i);

            kpm_sub_data_t kpm_sub = gen_kpm_subs(&n->rf[idx].defn.kpm);
            kpm_handles[i] = report_sm_xapp_api(&n->id, KPM_ran_function, &kpm_sub, sm_cb_kpm);
            assert(kpm_handles[i].success == true);
            free_kpm_sub_data(&kpm_sub);
        }
    }

    xapp_wait_end_api();

    // Cleanup
    printf("[Timer Handover xApp]: Starting cleanup...\n");

    for (int i = 0; i < nodes.len; ++i)
    {
        if (kpm_handles[i].success == true) {
            printf("[Cleanup] Removing KPM subscription for node %d\n", i);
            rm_report_sm_xapp_api(kpm_handles[i].u.handle);
        }
    }

    printf("[Timer Handover xApp]: Cleanup completed\n");

    // Reset della variabile globale
    global_nodes = NULL;

    free(kpm_handles);

    free_e2_node_arr_xapp(&nodes);

    int rc = pthread_mutex_destroy(&ho_timer_mutex);
    assert(rc == 0);

    printf("[Timer Handover xApp]: Test xApp completed successfully\n");
    return 0;
}
