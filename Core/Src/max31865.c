/**
 ******************************************************************************
 * @file    max31865.c
 * @brief   Driver MAX31865 (RTD-to-digital) — conversioni e interfaccia SPI2.
 ******************************************************************************
 */
#include "max31865.h"
#include "main.h"
#include "spi.h"

#define MAX31865_SPI_TIMEOUT_MS 50U

/* Registri (indirizzo di lettura; per la scrittura si somma 0x80) */
#define REG_CONFIG        0x00U
#define REG_RTD_MSB       0x01U
#define REG_FAULT_STATUS  0x07U

/* Bit del registro di configurazione */
#define CFG_VBIAS         0x80U
#define CFG_AUTO_CONV     0x40U
#define CFG_3WIRE         0x10U
#define CFG_FAULT_CLEAR   0x02U
#define CFG_FILTER_50HZ   0x01U

static MAX31865_Cal_t cal = {
    .rtd_r0_ohm = 100.0f,
    .rref_ohm = 430.0f,
    .wires_3 = true,
    .filter_50hz = false,
};

void MAX31865_SetCal(const MAX31865_Cal_t *c) {
  if (c != NULL) {
    cal = *c;
  }
}

const MAX31865_Cal_t *MAX31865_GetCal(void) { return &cal; }

/* ── Conversioni (pure) ─────────────────────────────────────────────────── */
float MAX31865_CodeToOhms(uint16_t code) {
  return ((float)code / 32768.0f) * cal.rref_ohm;
}

/* Radice quadrata via Newton-Raphson: evita di dipendere da sqrtf/libm,
 * non sempre linkata di default nelle build Debug (-O0) di progetti
 * STM32CubeIDE che non abilitano esplicitamente -lm. */
static float sqrt_newton(float x) {
  if (x <= 0.0f) {
    return 0.0f;
  }
  float guess = x;
  for (int i = 0; i < 16; i++) {
    guess = 0.5f * (guess + x / guess);
  }
  return guess;
}

float MAX31865_OhmsToCelsius(float ohms) {
  /* IEC 60751, coefficienti Callendar-Van Dusen (A, B), forma quadratica
   * valida per T >= 0 C. Sotto zero servirebbe il termine cubico C: non
   * implementato, fuori dal range applicativo di un riscaldatore. */
  const float A = 3.9083e-3f;
  const float B = -5.775e-7f;
  float r_ratio = ohms / cal.rtd_r0_ohm;
  float disc = (A * A) - (4.0f * B * (1.0f - r_ratio));
  if (disc < 0.0f) {
    disc = 0.0f;
  }
  return (-A + sqrt_newton(disc)) / (2.0f * B);
}

/* ── Interfaccia hardware ───────────────────────────────────────────────── */
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
      HAL_SPI_Transmit(&hspi3, tx, sizeof(tx), MAX31865_SPI_TIMEOUT_MS);
  cs_high();
  return st == HAL_OK;
}

static bool read_regs(uint8_t addr, uint8_t *buf, uint16_t len) {
  uint8_t tx = (uint8_t)(addr & 0x7FU);
  cs_low();
  HAL_StatusTypeDef st =
      HAL_SPI_Transmit(&hspi3, &tx, 1U, MAX31865_SPI_TIMEOUT_MS);
  if (st == HAL_OK) {
    st = HAL_SPI_Receive(&hspi3, buf, len, MAX31865_SPI_TIMEOUT_MS);
  }
  cs_high();
  return st == HAL_OK;
}

static uint8_t config_byte(void) {
  uint8_t cfg = CFG_VBIAS | CFG_AUTO_CONV;
  if (cal.wires_3) {
    cfg |= CFG_3WIRE;
  }
  if (cal.filter_50hz) {
    cfg |= CFG_FILTER_50HZ;
  }
  return cfg;
}

bool MAX31865_Init(void) {
  cs_high();
  return write_reg(REG_CONFIG, config_byte());
}

bool MAX31865_DataReady(void) {
  /* DRDY e' open-drain attivo-basso: LOW = nuova conversione pronta. */
  return HAL_GPIO_ReadPin(MAX31865_DRDY_GPIO_Port, MAX31865_DRDY_Pin) ==
         GPIO_PIN_RESET;
}

bool MAX31865_ReadRtd(uint16_t *code_out) {
  uint8_t buf[2];
  if (!read_regs(REG_RTD_MSB, buf, sizeof(buf))) {
    return false;
  }
  uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
  if (code_out != NULL) {
    *code_out = raw >> 1; /* bit0 di LSB e' il flag di fault, non dato ADC */
  }
  return true;
}

bool MAX31865_ReadFaultStatus(MAX31865_Fault_t *fault_out) {
  uint8_t raw;
  if (!read_regs(REG_FAULT_STATUS, &raw, 1U)) {
    return false;
  }
  if (fault_out != NULL) {
    fault_out->raw = raw;
    fault_out->rtd_high_thresh = (raw & 0x80U) != 0U;
    fault_out->rtd_low_thresh = (raw & 0x40U) != 0U;
    fault_out->refin_high = (raw & 0x20U) != 0U;
    fault_out->refin_low_forceopen = (raw & 0x10U) != 0U;
    fault_out->rtdin_low_forceopen = (raw & 0x08U) != 0U;
    fault_out->overvoltage_undervoltage = (raw & 0x04U) != 0U;
  }
  return true;
}

bool MAX31865_ClearFault(void) {
  return write_reg(REG_CONFIG, config_byte() | CFG_FAULT_CLEAR);
}
