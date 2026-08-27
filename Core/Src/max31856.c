/**
 ******************************************************************************
 * @file    max31856.c
 * @brief   Driver MAX31856 (termocoppia-to-digital) — interfaccia SPI1 (CN7).
 ******************************************************************************
 */
#include "max31856.h"
#include "main.h"
#include "spi.h"
#include "iwdg.h"

#define MAX31856_SPI_TIMEOUT_MS 50U

/* Registri (indirizzo di lettura; per la scrittura si somma 0x80) */
#define REG_CR0    0x00U
#define REG_CR1    0x01U
#define REG_MASK   0x02U
#define REG_LTCBH  0x0CU /* temperatura linearizzata, 3 byte: MSB/mid/LSB */
#define REG_SR     0x0FU /* fault status, sola lettura */

/* Bit del registro CR0 */
#define CR0_AUTOCONVERT  0x80U
#define CR0_OCFAULT0     0x10U /* OCFAULT[1:0]=01: rilevamento open-circuit
                                 * attivo, per ingresso senza cap RC esterno */
#define CR0_FAULT_CLEAR  0x02U
#define CR0_FILTER_50HZ  0x01U

static MAX31856_Cal_t cal = {
    .tc_type = MAX31856_TC_TYPE_T,
    .filter_50hz = false,
};

void MAX31856_SetCal(const MAX31856_Cal_t *c) {
  if (c != NULL) {
    cal = *c;
  }
}

const MAX31856_Cal_t *MAX31856_GetCal(void) { return &cal; }

/* ── Interfaccia hardware ───────────────────────────────────────────────── */
/* CS sul pin PD14 (CN7). */
static void cs_low(void) {
  HAL_GPIO_WritePin(MAX31856_CS_GPIO_Port, MAX31856_CS_Pin, GPIO_PIN_RESET);
}

static void cs_high(void) {
  HAL_GPIO_WritePin(MAX31856_CS_GPIO_Port, MAX31856_CS_Pin, GPIO_PIN_SET);
}

static bool write_reg(uint8_t addr, uint8_t data) {
  uint8_t tx[2] = {(uint8_t)(0x80U | addr), data};
  cs_low();
  HAL_StatusTypeDef st =
      HAL_SPI_Transmit(&hspi1, tx, sizeof(tx), MAX31856_SPI_TIMEOUT_MS);
  cs_high();
  return st == HAL_OK;
}

/* Indirizzo e dati in un'unica HAL_SPI_TransmitReceive(): due chiamate HAL
 * separate (Transmit poi Receive) con CS tenuto basso in mezzo lasciavano
 * al periferico SPI di H7 (v3.5) una finestra per re-inizializzare lo stato
 * delle linee tra le due transazioni (MasterKeepIOState=DISABLE), iniettando
 * un glitch che corrompeva l'indirizzo del registro visto dal chip — stesso
 * sintomo osservato identico su SPI3 e SPI1, quindi non un problema di
 * cablaggio. Un'unica transazione elimina il gap alla radice. */
static bool read_regs(uint8_t addr, uint8_t *buf, uint16_t len) {
  uint8_t tx[1U + 8U] = {0};
  uint8_t rx[1U + 8U] = {0};
  if (len > 8U) {
    return false;
  }
  tx[0] = (uint8_t)(addr & 0x7FU);
  cs_low();
  HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(
      &hspi1, tx, rx, (uint16_t)(1U + len), MAX31856_SPI_TIMEOUT_MS);
  cs_high();
  if (st == HAL_OK) {
    for (uint16_t i = 0U; i < len; i++) {
      buf[i] = rx[1U + i];
    }
  }
  return st == HAL_OK;
}

static uint8_t cr0_byte(void) {
  uint8_t cr0 = CR0_AUTOCONVERT | CR0_OCFAULT0;
  if (cal.filter_50hz) {
    cr0 |= CR0_FILTER_50HZ;
  }
  return cr0;
}

#define MAX31856_INIT_RETRIES 5U

/* Scrive un registro e verifica con una rilettura; ritenta in caso di
 * mismatch. Osservato in campo (GET RAW) che una delle tre scritture
 * consecutive di MAX31856_Init() puo' "scivolare" su un registro adiacente
 * (es. il valore atteso in CR1 ritrovato in MASK) — sintomo di un glitch
 * elettrico/di timing sul bus SPI3, non di un bug nella sequenza logica.
 * HAL_Delay(1) fra un tentativo e l'altro per lasciar assestare il bus. */
static bool write_reg_verified(uint8_t addr, uint8_t data) {
  for (uint32_t attempt = 0U; attempt < MAX31856_INIT_RETRIES; attempt++) {
    uint8_t readback = 0xFFU;
    if (write_reg(addr, data) && read_regs(addr, &readback, 1U) &&
        readback == data) {
      return true;
    }
    HAL_Delay(1U);
  }
  return false;
}

bool MAX31856_Init(void) {
  cs_high();
  HAL_Delay(1U); /* dwell minimo prima della prima transazione */
  bool ok = write_reg_verified(REG_CR0, cr0_byte());
  /* CR1: averaging a 1 campione (bit7:4 = 0) + selezione tipo termocoppia */
  ok = write_reg_verified(REG_CR1, (uint8_t)(cal.tc_type & 0x0FU)) && ok;
  /* Smaschera tutti i fault nel registro SR (default di reset li maschera) */
  ok = write_reg_verified(REG_MASK, 0x00U) && ok;
  return ok;
}

bool MAX31856_ReadTempC(float *temp_c_out) {
  uint8_t buf[3];
  if (!read_regs(REG_LTCBH, buf, sizeof(buf))) {
    return false;
  }
  int32_t raw = ((int32_t)buf[0] << 16) | ((int32_t)buf[1] << 8) | buf[2];
  if ((raw & 0x800000) != 0) {
    raw |= (int32_t)0xFF000000U; /* sign-extend il campo a 24 bit */
  }
  raw >>= 5; /* i 5 bit meno significativi sono riservati (sempre 0) */
  if (temp_c_out != NULL) {
    *temp_c_out = (float)raw * 0.0078125f; /* risoluzione 1/128 C per LSB */
  }
  return true;
}

bool MAX31856_ReadFaultStatus(MAX31856_Fault_t *fault_out) {
  uint8_t raw;
  if (!read_regs(REG_SR, &raw, 1U)) {
    return false;
  }
  if (fault_out != NULL) {
    fault_out->raw = raw;
    fault_out->cj_range = (raw & 0x80U) != 0U;
    fault_out->tc_range = (raw & 0x40U) != 0U;
    fault_out->cj_high = (raw & 0x20U) != 0U;
    fault_out->cj_low = (raw & 0x10U) != 0U;
    fault_out->tc_high = (raw & 0x08U) != 0U;
    fault_out->tc_low = (raw & 0x04U) != 0U;
    fault_out->overvoltage_undervoltage = (raw & 0x02U) != 0U;
    fault_out->open_circuit = (raw & 0x01U) != 0U;
  }
  return true;
}

bool MAX31856_ClearFault(void) {
  return write_reg(REG_CR0, cr0_byte() | CR0_FAULT_CLEAR);
}

void MAX31856_ReadGpioLevels(MAX31856_GpioLevels_t *out) {
  if (out == NULL) {
    return;
  }
  out->cs = HAL_GPIO_ReadPin(MAX31856_CS_GPIO_Port, MAX31856_CS_Pin) == GPIO_PIN_SET;
  out->sck = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_5) == GPIO_PIN_SET;
  out->miso = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == GPIO_PIN_SET;
  out->mosi = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET;
}

void MAX31856_TestSckToggle(void) {
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);

  uint32_t until = HAL_GetTick() + 2000U; /* 2 s: visibile anche senza trigger */
  while (HAL_GetTick() < until) {
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    HAL_IWDG_Refresh(&hiwdg1); /* IWDG1 ~2s: senza refresh qui l'MCU si resetta a meta' test */
    HAL_Delay(1); /* ~500 Hz */
  }

  /* ripristina la funzione SPI1 AF5 */
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOA, &gpio);
}

void MAX31856_TestMosiToggle(void) {
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gpio);

  uint32_t until = HAL_GetTick() + 2000U;
  while (HAL_GetTick() < until) {
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_5);
    HAL_IWDG_Refresh(&hiwdg1);
    HAL_Delay(1);
  }

  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOB, &gpio);
}

uint32_t MAX31856_TestLoopback(void) {
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gpio); /* MOSI come uscita manuale */

  uint32_t matches = 0U;
  for (uint32_t i = 0U; i < MAX31856_LOOPBACK_SAMPLES; i++) {
    GPIO_PinState forced = (i % 2U == 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, forced);
    HAL_Delay(2);
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == forced) {
      matches++;
    }
    HAL_IWDG_Refresh(&hiwdg1);
  }

  /* ripristina la funzione SPI1 AF5 su MOSI */
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Alternate = GPIO_AF5_SPI1;
  HAL_GPIO_Init(GPIOB, &gpio);

  return matches;
}

bool MAX31856_ReadRaw(MAX31856_RawDump_t *out) {
  if (out == NULL) {
    return false;
  }
  bool ok = read_regs(REG_CR0, &out->cr0, 1U);
  ok = read_regs(REG_CR1, &out->cr1, 1U) && ok;
  ok = read_regs(REG_MASK, &out->mask, 1U) && ok;
  ok = read_regs(REG_SR, &out->sr, 1U) && ok;
  ok = read_regs(REG_LTCBH, out->ltcb, sizeof(out->ltcb)) && ok;
  out->ok = ok;
  return ok;
}
