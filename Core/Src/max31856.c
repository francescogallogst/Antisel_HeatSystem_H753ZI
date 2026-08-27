/**
 ******************************************************************************
 * @file    max31856.c
 * @brief   Driver MAX31856 (termocoppia-to-digital) — interfaccia SPI3.
 ******************************************************************************
 */
#include "max31856.h"
#include "main.h"
#include "spi.h"

#define MAX31856_SPI_TIMEOUT_MS 50U

/* Registri (indirizzo di lettura; per la scrittura si somma 0x80) */
#define REG_CR0    0x00U
#define REG_CR1    0x01U
#define REG_MASK   0x02U
#define REG_LTCBH  0x0CU /* temperatura linearizzata, 3 byte: MSB/mid/LSB */
#define REG_SR     0x0FU /* fault status, sola lettura */

/* Bit del registro CR0 */
#define CR0_AUTOCONVERT  0x80U
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
/* CS sul pin PE4 (etichetta CubeMX ereditata dal driver RTD precedente,
 * vedi docs/Mappatura_Pin_HeatSystem.md). */
static void cs_low(void) {
  HAL_GPIO_WritePin(MAX31865_CS_GPIO_Port, MAX31865_CS_Pin, GPIO_PIN_RESET);
}

static void cs_high(void) {
  HAL_GPIO_WritePin(MAX31865_CS_GPIO_Port, MAX31865_CS_Pin, GPIO_PIN_SET);
}

static bool write_reg(uint8_t addr, uint8_t data) {
  uint8_t tx[2] = {(uint8_t)(0x80U | addr), data};
  cs_low();
  HAL_StatusTypeDef st =
      HAL_SPI_Transmit(&hspi3, tx, sizeof(tx), MAX31856_SPI_TIMEOUT_MS);
  cs_high();
  return st == HAL_OK;
}

static bool read_regs(uint8_t addr, uint8_t *buf, uint16_t len) {
  uint8_t tx = (uint8_t)(addr & 0x7FU);
  cs_low();
  HAL_StatusTypeDef st =
      HAL_SPI_Transmit(&hspi3, &tx, 1U, MAX31856_SPI_TIMEOUT_MS);
  if (st == HAL_OK) {
    st = HAL_SPI_Receive(&hspi3, buf, len, MAX31856_SPI_TIMEOUT_MS);
  }
  cs_high();
  return st == HAL_OK;
}

static uint8_t cr0_byte(void) {
  uint8_t cr0 = CR0_AUTOCONVERT;
  if (cal.filter_50hz) {
    cr0 |= CR0_FILTER_50HZ;
  }
  return cr0;
}

bool MAX31856_Init(void) {
  cs_high();
  bool ok = write_reg(REG_CR0, cr0_byte());
  /* CR1: averaging a 1 campione (bit7:4 = 0) + selezione tipo termocoppia */
  ok = write_reg(REG_CR1, (uint8_t)(cal.tc_type & 0x0FU)) && ok;
  /* Smaschera tutti i fault nel registro SR (default di reset li maschera) */
  ok = write_reg(REG_MASK, 0x00U) && ok;
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
