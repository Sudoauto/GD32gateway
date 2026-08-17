#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "gd32h7xx.h"

/* ========================================================================== */
/* Debug console: USART2, TX=PC10, RX=PC11                                    */
/* ========================================================================== */
#define GW_DEBUG_USART                    USART2
#define GW_DEBUG_USART_RCU                RCU_USART2
#define GW_DEBUG_GPIO_PORT                GPIOC
#define GW_DEBUG_GPIO_RCU                 RCU_GPIOC
#define GW_DEBUG_TX_PIN                   GPIO_PIN_10
#define GW_DEBUG_RX_PIN                   GPIO_PIN_11
#define GW_DEBUG_GPIO_AF                  GPIO_AF_7

/* ========================================================================== */
/* On-board RS485: UART4, TX=PB6, RX=PB12, DE=/RE=PB4                         */
/* Transceiver on the EmbedFire BTB baseboard: SIT3088EESA                    */
/* ========================================================================== */
#define GW_RS485_UART                     UART4
#define GW_RS485_UART_RCU                 RCU_UART4
#define GW_RS485_GPIO_PORT                GPIOB
#define GW_RS485_GPIO_RCU                 RCU_GPIOB
#define GW_RS485_TX_PIN                   GPIO_PIN_6
#define GW_RS485_RX_PIN                   GPIO_PIN_12
#define GW_RS485_GPIO_AF                  GPIO_AF_14

#define GW_RS485_DE_PORT                  GPIOB
#define GW_RS485_DE_RCU                   RCU_GPIOB
#define GW_RS485_DE_PIN                   GPIO_PIN_4

/* Dedicated DMA channels for first-batch RS485 driver. DMAMUX selects UART4. */
#define GW_RS485_DMA                      DMA0
#define GW_RS485_DMA_RCU                  RCU_DMA0
#define GW_RS485_TX_DMA_CH                DMA_CH0
#define GW_RS485_RX_DMA_CH                DMA_CH1
#define GW_RS485_TX_DMA_REQUEST           DMA_REQUEST_UART4_TX
#define GW_RS485_RX_DMA_REQUEST           DMA_REQUEST_UART4_RX

#define GW_RS485_UART_IRQn                UART4_IRQn
#define GW_RS485_TX_DMA_IRQn              DMA0_Channel0_IRQn
#define GW_RS485_RX_DMA_IRQn              DMA0_Channel1_IRQn

#define GW_RS485_RX_DMA_BUFFER_SIZE       256U
#define GW_RS485_TX_DMA_BUFFER_SIZE       320U

/* ========================================================================== */
/* CAN-FD: CAN2, TX=PD13, RX=PD12, transceiver=SIT1042AQT/3                  */
/* ========================================================================== */
#define GW_CANFD_CAN                       CAN2
#define GW_CANFD_CAN_RCU                   RCU_CAN2
#define GW_CANFD_GPIO_PORT                 GPIOD
#define GW_CANFD_GPIO_RCU                  RCU_GPIOD
#define GW_CANFD_RX_PIN                    GPIO_PIN_12
#define GW_CANFD_TX_PIN                    GPIO_PIN_13
#define GW_CANFD_GPIO_AF                   GPIO_AF_5

#define GW_CANFD_MESSAGE_IRQn              CAN2_Message_IRQn
#define GW_CANFD_BUSOFF_IRQn               CAN2_Busoff_IRQn
#define GW_CANFD_ERROR_IRQn                CAN2_Error_IRQn
#define GW_CANFD_FAST_ERROR_IRQn           CAN2_FastError_IRQn

#endif
