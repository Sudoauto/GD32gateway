#include "gw_diagnostics.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "can_decoder.h"
#include "drv_canfd.h"
#include "drv_rs485.h"
#include "gateway_build_config.h"
#include "gw_config.h"
#include "gw_ethernetif.h"
#include "gw_time.h"
#include "task_rs485.h"

#if (GW_DIAGNOSTICS_ENABLE!=0U)
#define DIAG_STACK_WORDS 1024U
#define DIAG_PRIORITY 1U
static StaticTask_t s_tcb;static StackType_t s_stack[DIAG_STACK_WORDS];static gw_diag_sample_t s_hist[GW_DIAG_HISTORY_DEPTH];static uint32_t s_head,s_count;static gw_diag_status_t s_status;static gw_selftest_status_t s_selftest;
static uint32_t s_prev_can_bytes,s_prev_can_frames,s_prev_rs_bytes,s_prev_attempts,s_prev_errors;static uint32_t s_prev_idle_runtime,s_prev_total_runtime;

static uint16_t cpu_load(void)
{
    TaskStatus_t st[32];uint32_t total=0U;UBaseType_t n=uxTaskGetSystemState(st,32U,&total);TaskHandle_t idle=xTaskGetIdleTaskHandle();uint32_t idle_rt=0U;for(UBaseType_t i=0;i<n;++i)if(st[i].xHandle==idle){idle_rt=st[i].ulRunTimeCounter;break;}
    uint32_t dt=total-s_prev_total_runtime,di=idle_rt-s_prev_idle_runtime;s_prev_total_runtime=total;s_prev_idle_runtime=idle_rt;if(dt==0U)return s_status.cpu_load_permille;uint32_t busy=(dt>di)?dt-di:0U;uint32_t p=(busy*1000U)/dt;return (uint16_t)((p>1000U)?1000U:p);
}
static void sample_once(void)
{
    canfd_stats_t c;rs485_dma_stats_t rd;rs485_task_stats_t rt;gw_ethernetif_stats_t e;memset(&c,0,sizeof(c));memset(&rd,0,sizeof(rd));memset(&rt,0,sizeof(rt));memset(&e,0,sizeof(e));drv_canfd_get_stats(&c);drv_rs485_get_stats(&rd);task_rs485_get_stats(&rt);gw_ethernetif_get_stats(&e);
    uint32_t can_bytes=c.rx_bytes+c.tx_bytes,can_frames=c.rx_frames+c.tx_success;uint32_t dc_bytes=can_bytes-s_prev_can_bytes,dc_frames=can_frames-s_prev_can_frames;s_prev_can_bytes=can_bytes;s_prev_can_frames=can_frames;
    /* Approximate on-wire bits at 500k: payload bits plus a conservative 80-bit
     * framing/stuffing allowance per FD frame. It is intended as an HMI trend,
     * not a protocol-analyzer measurement. */
    uint32_t can_bits=dc_bytes*8U+dc_frames*80U;uint32_t can_cap=(500000U*GW_DIAG_SAMPLE_MS)/1000U;uint32_t cl=(can_cap!=0U)?(can_bits*1000U)/can_cap:0U;if(cl>1000U)cl=1000U;
    uint32_t rs_bytes=rd.tx_bytes+rd.rx_bytes,drs=rs_bytes-s_prev_rs_bytes;s_prev_rs_bytes=rs_bytes;gw_runtime_config_t cfg;gw_config_get_runtime(&cfg);uint32_t rs_cap=(cfg.rs485_baudrate*GW_DIAG_SAMPLE_MS)/1000U;uint32_t rl=(rs_cap!=0U)?((drs*11U)*1000U)/rs_cap:0U;if(rl>1000U)rl=1000U;
    uint32_t attempts=rt.attempt_count,errors=rt.final_timeout_count+rt.final_crc_count+rt.final_protocol_count+rt.final_io_count;uint32_t da=attempts-s_prev_attempts,de=errors-s_prev_errors;s_prev_attempts=attempts;s_prev_errors=errors;uint32_t loss=(da!=0U)?(de*1000U)/da:0U;if(loss>1000U)loss=1000U;
    gw_diag_sample_t x;memset(&x,0,sizeof(x));x.timestamp_ms=gw_time_utc_ms();if(x.timestamp_ms==0U)x.timestamp_ms=gw_time_ms();x.cpu_load_permille=cpu_load();x.can_load_permille=(uint16_t)cl;x.rs485_load_permille=(uint16_t)rl;x.rs485_loss_permille=(uint16_t)loss;x.free_heap_bytes=(uint32_t)xPortGetFreeHeapSize();x.can_error_total=c.ack_error_count+c.stuff_error_count+c.form_error_count+c.crc_error_count+c.bit_error_count+c.busoff_count;x.rs485_error_total=errors;x.eth_rx_frames=e.rx_frames;x.eth_tx_frames=e.tx_frames;
    taskENTER_CRITICAL();
    s_hist[s_head]=x;s_head=(s_head+1U)%GW_DIAG_HISTORY_DEPTH;if(s_count<GW_DIAG_HISTORY_DEPTH)++s_count;s_status.running=true;++s_status.sample_count;s_status.cpu_load_permille=x.cpu_load_permille;s_status.can_load_permille=x.can_load_permille;s_status.rs485_load_permille=x.rs485_load_permille;s_status.rs485_loss_permille=x.rs485_loss_permille;s_status.free_heap_bytes=x.free_heap_bytes;
    taskEXIT_CRITICAL();
}
void gw_diagnostics_init(void){memset(s_hist,0,sizeof(s_hist));memset(&s_status,0,sizeof(s_status));memset(&s_selftest,0,sizeof(s_selftest));s_head=s_count=0U;s_prev_can_bytes=s_prev_can_frames=s_prev_rs_bytes=s_prev_attempts=s_prev_errors=s_prev_idle_runtime=s_prev_total_runtime=0U;}
static void diag_task(void*a){(void)a;TickType_t last=xTaskGetTickCount();for(;;){sample_once();vTaskDelayUntil(&last,pdMS_TO_TICKS(GW_DIAG_SAMPLE_MS));}}
void gw_diagnostics_task_create(void){TaskHandle_t h=xTaskCreateStatic(diag_task,"diag",DIAG_STACK_WORDS,NULL,DIAG_PRIORITY,s_stack,&s_tcb);configASSERT(h!=NULL);}
void gw_diagnostics_get_status(gw_diag_status_t*out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_status;taskEXIT_CRITICAL();}
uint32_t gw_diagnostics_history(gw_diag_sample_t*out,uint32_t max){if((out==NULL)||(max==0U))return 0U;taskENTER_CRITICAL();uint32_t n=(s_count<max)?s_count:max;uint32_t start=(s_head+GW_DIAG_HISTORY_DEPTH-n)%GW_DIAG_HISTORY_DEPTH;for(uint32_t i=0;i<n;++i)out[i]=s_hist[(start+i)%GW_DIAG_HISTORY_DEPTH];taskEXIT_CRITICAL();return n;}
__attribute__((weak)) gw_err_t gw_diag_fixture_can_loopback(void){return GW_ERR_NOT_SUPPORTED;}
__attribute__((weak)) gw_err_t gw_diag_fixture_rs485_loopback(void){return GW_ERR_NOT_SUPPORTED;}
static gw_selftest_state_t fixture_state(gw_err_t e){return (e==GW_OK)?GW_SELFTEST_PASS:(e==GW_ERR_NOT_SUPPORTED)?GW_SELFTEST_NOT_SUPPORTED:GW_SELFTEST_FAIL;}
void gw_diagnostics_run_selftest(void){
    /* First run non-destructive driver health checks. Active electrical tests
     * are delegated to optional board fixture hooks so a live field bus is
     * never shorted or driven into itself accidentally. */
    canfd_stats_t c;rs485_dma_stats_t r;gw_selftest_status_t st;memset(&st,0,sizeof(st));drv_canfd_get_stats(&c);drv_rs485_get_stats(&r);st.timestamp_ms=gw_time_ms();
    st.can_result=(c.busoff_count==0U&&c.rx_read_error==0U)?GW_OK:GW_ERR_IO;st.can_state=(st.can_result==GW_OK)?GW_SELFTEST_PASS:GW_SELFTEST_FAIL;
    st.rs485_result=(r.dma_error_count==0U&&r.uart_error_count==0U)?GW_OK:GW_ERR_IO;st.rs485_state=(st.rs485_result==GW_OK)?GW_SELFTEST_PASS:GW_SELFTEST_FAIL;
    st.can_fixture_result=gw_diag_fixture_can_loopback();st.can_fixture_state=fixture_state(st.can_fixture_result);
    st.rs485_fixture_result=gw_diag_fixture_rs485_loopback();st.rs485_fixture_state=fixture_state(st.rs485_fixture_result);
    taskENTER_CRITICAL();s_selftest=st;taskEXIT_CRITICAL();
}
void gw_diagnostics_get_selftest(gw_selftest_status_t*out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_selftest;taskEXIT_CRITICAL();}
uint32_t gw_diagnostics_metric_u32(uint16_t id){gw_diag_sample_t x;memset(&x,0,sizeof(x));if(gw_diagnostics_history(&x,1U)==0U)return 0U;switch(id){case 1:return (uint32_t)(gw_time_ms()/1000U);case 2:return x.cpu_load_permille;case 3:return x.free_heap_bytes;case 10:return x.eth_rx_frames;case 11:return x.eth_tx_frames;case 20:return x.can_load_permille;case 21:return x.can_error_total;case 30:return x.rs485_load_permille;case 31:return x.rs485_loss_permille;case 32:return x.rs485_error_total;default:return 0U;}}
#else
void gw_diagnostics_init(void){}void gw_diagnostics_task_create(void){}void gw_diagnostics_get_status(gw_diag_status_t*o){if(o)memset(o,0,sizeof(*o));}uint32_t gw_diagnostics_history(gw_diag_sample_t*o,uint32_t m){(void)o;(void)m;return 0U;}void gw_diagnostics_run_selftest(void){}void gw_diagnostics_get_selftest(gw_selftest_status_t*o){if(o)memset(o,0,sizeof(*o));}uint32_t gw_diagnostics_metric_u32(uint16_t i){(void)i;return 0U;}__attribute__((weak)) gw_err_t gw_diag_fixture_can_loopback(void){return GW_ERR_NOT_SUPPORTED;}__attribute__((weak)) gw_err_t gw_diag_fixture_rs485_loopback(void){return GW_ERR_NOT_SUPPORTED;}
#endif
