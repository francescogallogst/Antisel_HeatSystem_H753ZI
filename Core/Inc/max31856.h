/**
 ******************************************************************************
 * @file    max31856.h
 * @brief   Driver MAX31856 (termocoppia-to-digital) — interfaccia SPI3.
 *          Sostituisce max31865.h: l'hardware montato (Adafruit MAX31856,
 *          termocoppia Tipo T) non e' un MAX31865 (RTD), protocollo/registri
 *          incompatibili tra i due chip.
 ******************************************************************************
 */
#ifndef MAX31856_H
#define MAX31856_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  MAX31856_TC_TYPE_B = 0x0,
  MAX31856_TC_TYPE_E = 0x1,
  MAX31856_TC_TYPE_J = 0x2,
  MAX31856_TC_TYPE_K = 0x3,
  MAX31856_TC_TYPE_N = 0x4,
  MAX31856_TC_TYPE_R = 0x5,
  MAX31856_TC_TYPE_S = 0x6,
  MAX31856_TC_TYPE_T = 0x7,
} MAX31856_TcType_t;

typedef struct {
  MAX31856_TcType_t tc_type;
  bool filter_50hz;   /* false = reiezione 60 Hz, true = 50 Hz (rete IT) */
} MAX31856_Cal_t;

typedef struct {
  uint8_t raw;
  bool cj_range;
  bool tc_range;
  bool cj_high;
  bool cj_low;
  bool tc_high;
  bool tc_low;
  bool overvoltage_undervoltage;
  bool open_circuit;
} MAX31856_Fault_t;

/* Calibrazione -------------------------------------------------------------*/
void MAX31856_SetCal(const MAX31856_Cal_t *cal);
const MAX31856_Cal_t *MAX31856_GetCal(void);

/* Interfaccia hardware -------------------------------------------------------
 * Modo di funzionamento: conversione automatica continua (CMODE=1), stessa
 * filosofia del driver precedente per il polling periodico dal loop
 * principale. Linearizzazione e compensazione giunto freddo eseguite
 * internamente dal chip: nessuna conversione lato firmware necessaria. */
bool MAX31856_Init(void);

/* Legge la temperatura linearizzata (LTCB) gia' in gradi Celsius */
bool MAX31856_ReadTempC(float *temp_c_out);

bool MAX31856_ReadFaultStatus(MAX31856_Fault_t *fault_out);
bool MAX31856_ClearFault(void);

/* Diagnostica: byte grezzi cosi' come arrivano dal chip via SPI, prima di
 * qualunque conversione/interpretazione — utile per verificare se il
 * problema e' nel chip/collegamento (registri a valori di reset o
 * incoerenti) o nella conversione lato firmware. */
typedef struct {
  bool ok;        /* false se una qualunque transazione SPI e' fallita */
  uint8_t cr0;
  uint8_t cr1;
  uint8_t mask;
  uint8_t sr;      /* registro fault (0x0F) */
  uint8_t ltcb[3]; /* LTCBH/LTCBM/LTCBL (0x0C-0x0E), stesso raw di ReadTempC */
} MAX31856_RawDump_t;

bool MAX31856_ReadRaw(MAX31856_RawDump_t *out);

#endif /* MAX31856_H */
