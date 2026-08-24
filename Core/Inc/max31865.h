/**
 ******************************************************************************
 * @file    max31865.h
 * @brief   Driver MAX31865 (RTD-to-digital) — conversioni e interfaccia SPI2.
 *          Stessa forma di ina301.c/.h in AntiSEL.
 ******************************************************************************
 */
#ifndef MAX31865_H
#define MAX31865_H

#include <stdint.h>
#include <stdbool.h>

/* Calibrazione/wiring del front-end RTD (§7 proposta: TBD, default
 * ragionevoli — PT100 3 fili su breakout con R_REF=430ohm, tipico per
 * moduli MAX31865 commerciali). Da confermare/adeguare all'hardware reale. */
typedef struct {
  float rtd_r0_ohm;   /* resistenza RTD a 0 C: 100 = PT100, 1000 = PT1000 */
  float rref_ohm;     /* resistenza di riferimento sul breakout MAX31865 */
  bool wires_3;        /* true = collegamento 3 fili, false = 2/4 fili */
  bool filter_50hz;    /* false = reiezione 60 Hz, true = 50 Hz (rete IT) */
} MAX31865_Cal_t;

typedef struct {
  uint8_t raw;
  bool rtd_high_thresh;
  bool rtd_low_thresh;
  bool refin_high;
  bool refin_low_forceopen;
  bool rtdin_low_forceopen;
  bool overvoltage_undervoltage;
} MAX31865_Fault_t;

/* Calibrazione -------------------------------------------------------------*/
void MAX31865_SetCal(const MAX31865_Cal_t *cal);
const MAX31865_Cal_t *MAX31865_GetCal(void);

/* Conversioni (pure, testabili) ---------------------------------------------*/
float MAX31865_CodeToOhms(uint16_t code);
/* Callendar-Van Dusen quadratica, valida per T >= 0 C (range applicativo
 * di un riscaldatore: ambiente..+85 C circa) */
float MAX31865_OhmsToCelsius(float ohms);

/* Interfaccia hardware -------------------------------------------------------
 * Modo di funzionamento: VBIAS sempre attivo + conversione automatica
 * continua (~50/60 Hz interni). Piu' semplice/robusto per un polling
 * periodico dal loop principale rispetto al 1-shot con gestione dei tempi
 * di assestamento VBIAS; il costo e' un self-heating leggermente maggiore,
 * accettabile per il controllo di un riscaldatore (non e' una misura di
 * precisione metrologica). */
bool MAX31865_Init(void);

/* true se e' disponibile una nuova conversione (DRDY basso, attivo-basso) */
bool MAX31865_DataReady(void);

/* Legge il codice RTD a 15 bit (gia' depurato dal bit di fault in LSB) */
bool MAX31865_ReadRtd(uint16_t *code_out);

bool MAX31865_ReadFaultStatus(MAX31865_Fault_t *fault_out);
bool MAX31865_ClearFault(void);

#endif /* MAX31865_H */
