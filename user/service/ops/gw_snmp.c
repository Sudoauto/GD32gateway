#include "gw_snmp.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "gateway_build_config.h"
#include "gw_config.h"
#include "gw_diagnostics.h"
#include "rtos_objects.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"

#if ((GW_ETH_ENABLE!=0U)&&(GW_SNMP_ENABLE!=0U))
#define SNMP_STACK_WORDS 896U
#define SNMP_PRIORITY 1U
static StaticTask_t s_tcb;static StackType_t s_stack[SNMP_STACK_WORDS];static gw_snmp_stats_t s_stats;
typedef struct{uint8_t tag;const uint8_t*v;uint16_t len;}tlv_t;
static bool tlv_get(const uint8_t **p,uint16_t *remain,tlv_t*out){if((*remain<2U)||(p==NULL)||(out==NULL))return false;const uint8_t*q=*p;uint16_t n=q[1],hdr=2U;if((q[1]&0x80U)!=0U){uint8_t c=q[1]&0x7FU;if((c==0U)||(c>2U)||(*remain<(uint16_t)(2U+c)))return false;n=0U;for(uint8_t i=0;i<c;++i)n=(uint16_t)((n<<8U)|q[2U+i]);hdr=(uint16_t)(2U+c);}if((uint32_t)hdr+n>*remain)return false;out->tag=q[0];out->v=q+hdr;out->len=n;*p=q+hdr+n;*remain=(uint16_t)(*remain-hdr-n);return true;}
static uint16_t put_tlv(uint8_t*o,uint8_t tag,const uint8_t*v,uint16_t n){o[0]=tag;o[1]=(uint8_t)n;if(n)memcpy(o+2,v,n);return (uint16_t)(n+2U);}
static uint16_t uint_value(uint32_t v,uint8_t*out){uint8_t t[5];uint8_t n=0U;do{t[4U-n]=(uint8_t)v;v>>=8U;++n;}while(v!=0U&&n<4U);uint8_t start=(uint8_t)(5U-n);if((t[start]&0x80U)!=0U){t[--start]=0U;++n;}memcpy(out,&t[start],n);return n;}
static bool parse_metric(const uint8_t*oid,uint16_t n,uint16_t*metric){static const uint8_t prefix[]={0x2B,0x06,0x01,0x04,0x01,0x83,0xB2,0x03,0x01};if((n!=sizeof(prefix)+1U)||memcmp(oid,prefix,sizeof(prefix))!=0)return false;*metric=oid[sizeof(prefix)];return true;}
static uint16_t build_response(uint8_t*out,uint16_t cap,int version,const char*community,const uint8_t*reqid,uint16_t reqid_len,const uint8_t*oid,uint16_t oid_len,uint32_t value)
{uint8_t val[5];uint16_t vn=uint_value(value,val);uint8_t vb[64],vbl[70],pdu[96],outer[120];uint16_t u=0U;u+=put_tlv(vb+u,0x06,oid,oid_len);u+=put_tlv(vb+u,0x02,val,vn);uint16_t vbu=put_tlv(vbl,0x30,vb,u);uint8_t vblwrap[74];uint16_t vbln=put_tlv(vblwrap,0x30,vbl,vbu);u=0U;u+=put_tlv(pdu+u,0x02,reqid,reqid_len);uint8_t z=0U;u+=put_tlv(pdu+u,0x02,&z,1U);u+=put_tlv(pdu+u,0x02,&z,1U);memcpy(pdu+u,vblwrap,vbln);u+=vbln;uint8_t pdutlv[100];uint16_t pn=put_tlv(pdutlv,0xA2,pdu,u);u=0U;uint8_t ver=(uint8_t)version;u+=put_tlv(outer+u,0x02,&ver,1U);u+=put_tlv(outer+u,0x04,(const uint8_t*)community,(uint16_t)strlen(community));memcpy(outer+u,pdutlv,pn);u+=pn;if(u+2U>cap)return 0U;return put_tlv(out,0x30,outer,u);}
static uint16_t handle(const uint8_t*in,uint16_t len,uint8_t*out,uint16_t cap)
{const uint8_t*p=in;uint16_t rem=len;tlv_t outer;if(!tlv_get(&p,&rem,&outer)||outer.tag!=0x30)return 0U;p=outer.v;rem=outer.len;tlv_t ver,comm,pdu;if(!tlv_get(&p,&rem,&ver)||ver.tag!=0x02||ver.len!=1U||!tlv_get(&p,&rem,&comm)||comm.tag!=0x04||!tlv_get(&p,&rem,&pdu)||pdu.tag!=0xA0)return 0U;gw_runtime_config_t cfg;gw_config_get_runtime(&cfg);size_t cl=strlen(cfg.snmp_community);if(comm.len!=cl||memcmp(comm.v,cfg.snmp_community,cl)!=0){++s_stats.auth_reject;return 0U;}const uint8_t*q=pdu.v;uint16_t qr=pdu.len;tlv_t reqid,es,ei,vbl;if(!tlv_get(&q,&qr,&reqid)||reqid.tag!=0x02||!tlv_get(&q,&qr,&es)||!tlv_get(&q,&qr,&ei)||!tlv_get(&q,&qr,&vbl)||vbl.tag!=0x30)return 0U;q=vbl.v;qr=vbl.len;tlv_t vb;if(!tlv_get(&q,&qr,&vb)||vb.tag!=0x30)return 0U;q=vb.v;qr=vb.len;tlv_t oid,val;if(!tlv_get(&q,&qr,&oid)||oid.tag!=0x06||!tlv_get(&q,&qr,&val))return 0U;uint16_t metric;if(!parse_metric(oid.v,oid.len,&metric))return 0U;uint32_t mv=gw_diagnostics_metric_u32(metric);if(comm.len>=24U)return 0U;char community[24];memcpy(community,comm.v,comm.len);community[comm.len]='\0';return build_response(out,cap,ver.v[0],community,reqid.v,reqid.len,oid.v,oid.len,mv);}
void gw_snmp_init(void){memset(&s_stats,0,sizeof(s_stats));}
static void snmp_task(void*a){(void)a;struct netconn*c=netconn_new(NETCONN_UDP);if(c==NULL)vTaskDelete(NULL);if(netconn_bind(c,IP_ADDR_ANY,GW_SNMP_PORT)!=ERR_OK){netconn_delete(c);vTaskDelete(NULL);}netconn_set_recvtimeout(c,1000U);for(;;){struct netbuf*b=NULL;if(netconn_recv(c,&b)==ERR_OK&&b!=NULL){++s_stats.requests;void*d=NULL;u16_t n=0U;netbuf_data(b,&d,&n);uint8_t resp[128];uint16_t rn=handle((const uint8_t*)d,n,resp,sizeof(resp));if(rn!=0U){const ip_addr_t*addr=netbuf_fromaddr(b);u16_t port=netbuf_fromport(b);struct netbuf*rb=netbuf_new();void*rp=(rb!=NULL)?netbuf_alloc(rb,rn):NULL;if(rp!=NULL){memcpy(rp,resp,rn);if(netconn_sendto(c,rb,addr,port)==ERR_OK)++s_stats.responses;}if(rb)netbuf_delete(rb);}else ++s_stats.parse_error;netbuf_delete(b);}}}
void gw_snmp_task_create(void){TaskHandle_t h=xTaskCreateStatic(snmp_task,"snmp",SNMP_STACK_WORDS,NULL,SNMP_PRIORITY,s_stack,&s_tcb);configASSERT(h!=NULL);}void gw_snmp_get_stats(gw_snmp_stats_t*out){if(out==NULL)return;taskENTER_CRITICAL();*out=s_stats;taskEXIT_CRITICAL();}
#else
void gw_snmp_init(void){}void gw_snmp_task_create(void){}void gw_snmp_get_stats(gw_snmp_stats_t*o){if(o)memset(o,0,sizeof(*o));}
#endif
