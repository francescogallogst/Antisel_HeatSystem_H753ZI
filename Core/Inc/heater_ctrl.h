/**
 ******************************************************************************
 * @file    heater_ctrl.h
 * @brief   Controllo riscaldatore: PID + PWM (TIM4_CH4) + sicurezza locale.
 *          Proposta_HeatSystem_RTU_PID.md §5.2.
 ******************************************************************************
 */
#ifndef HEATER_CTRL_H
#define HEATER_CTRL_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  HEATER_OFF = 0,
  HEATER_MANUAL,
  HEATER_AUTO,
  HEATER_FAULT,
} HeaterMode_t;

typedef struct {
  float kp;
  float ki;
  float kd;
  float cutoff_c;        /* spegnimento hardware/software se T supera questo */
  uint32_t watchdog_ms;   /* AUTO senza comando/keepalive entro questo -> OFF */
  uint32_t rtd_stale_ms;  /* nessuna lettura RTD fresca entro questo -> fault */
} Heater_Cal_t;

void Heater_SetCal(const Heater_Cal_t *cal);
const Heater_Cal_t *Heater_GetCal(void);

void Heater_Init(void);

/* Da chiamare periodicamente dal loop principale (poll RTD, PID, safety,
 * watchdog, scrittura PWM). Non bloccante. */
void Heater_Service(void);

/* Comandi (aggiornano anche il timestamp del watchdog di disconnessione) */
void Heater_SetSetpointC(float setpoint_c);
void Heater_SetManualDuty(float duty_pct);
void Heater_Off(void);
void Heater_AckFault(void);

/* Stato per il protocollo RTU/PID */
float Heater_LastTempC(void);
float Heater_DutyPct(void);
HeaterMode_t Heater_GetMode(void);
const char *Heater_ModeName(HeaterMode_t mode);
bool Heater_RtdFresh(void);

#endif /* HEATER_CTRL_H */
