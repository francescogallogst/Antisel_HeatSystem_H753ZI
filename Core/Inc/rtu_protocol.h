/**
 ******************************************************************************
 * @file    rtu_protocol.h
 * @brief   Server TCP RTU/PID (porta 7756) — protocollo GET/SET testuale
 *          atteso da antisel_dashboard_eth.py (pannello "PID CTRL + RTU").
 ******************************************************************************
 */
#ifndef RTU_PROTOCOL_H
#define RTU_PROTOCOL_H

#define RTU_PROTOCOL_PORT 7756U

void RTU_Init(void);

#endif /* RTU_PROTOCOL_H */
