/**
 ******************************************************************************
 * @file    heater_ctrl.c
 * @brief   Controllo riscaldatore: PID + PWM (TIM4_CH4) + sicurezza locale.
 ******************************************************************************
 */
#include "heater_ctrl.h"
#include "main.h"
#include "spi.h"
#include "tim.h"
#include "max31856.h"

max31856_t hmax;

/* Guadagni PID, soglia di cutoff e timeout watchdog: valori di partenza
 * prudenti, NON tarati (Proposta_HeatSystem_RTU_PID.md §7, decisioni
 * ancora aperte). Da rivedere con hardware/attuatore reali. */
static Heater_Cal_t cal = {
    .kp = 5.0f,
    .ki = 0.05f,
    .kd = 0.0f,
    .cutoff_c = 90.0f,      /* margine sopra il target ~85 C citato in §1 */
    .watchdog_ms = 5000U,   /* multiplo del poll GUI (RTU_POLL_S = 1 s) */
    .rtd_stale_ms = 2000U,
};

static HeaterMode_t mode = HEATER_OFF;
static float setpoint_c = 25.0f;
static float duty_pct;
static float latest_temp_c;
static float manual_duty_pct;

static float pid_integral;
static float pid_prev_temp_c;
static uint32_t last_pid_tick;
static uint32_t last_rtd_tick;
static uint32_t last_rtd_ok_tick;
static bool rtd_ever_ok;
static uint32_t last_command_tick;

/* Variabili di stato per Autotuning */
static int autotune_state;
static float autotune_peak_high;
static float autotune_peak_low;
static uint32_t autotune_t1;
static uint32_t autotune_t2;
static uint32_t autotune_t3;
static int autotune_cycles;

void Heater_SetCal(const Heater_Cal_t *c) {
  if (c != NULL) {
    cal = *c;
  }
}

const Heater_Cal_t *Heater_GetCal(void) { return &cal; }

static void write_duty(float pct) {
  if (pct < 0.0f) {
    pct = 0.0f;
  }
  if (pct > 100.0f) {
    pct = 100.0f;
  }
  duty_pct = pct;
  uint32_t period = htim4.Init.Period; /* es. 65535 */
  uint32_t counts = (uint32_t)((pct / 100.0f) * (float)period);
  /* clamp esplicito: CCR e' un registro a 16 bit, un valore di 'period+1'
   * (100% "esatto") lo farebbe traboccare a 0, cioe' 0% invece di 100% */
  if (counts > period) {
    counts = period;
  }
  __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_4, counts);
}

void Heater_Init(void) {
  hmax.spi_handle = &hspi1;
  hmax.cs_pin.gpio_port = MAX31856_CS_GPIO_Port;
  hmax.cs_pin.gpio_pin = MAX31856_CS_Pin;

  uint8_t retries = 10;
  do {
    /* Direct configuration to avoid read-modify-write corruption 
     * CR0 = 0x95: Continuous (1), OC_FAULT R<5k (01), CJ Enabled (0), Fault Int (1), 50Hz (1)
     * CR1 = 0x07: 1 sample avg, Type T thermocouple
     * MASK = 0x00: All faults enabled
     */
    max31856_write_register(&hmax, MAX31856_CR0, 0x95);
    max31856_write_register(&hmax, MAX31856_CR1, 0x07);
    max31856_write_register(&hmax, MAX31856_MASK, 0x00);
    
    /* Read back CR0 to verify CONV_CONTINUOUS (bit 7) was correctly set */
    uint8_t cr0 = max31856_read_register(&hmax, MAX31856_CR0);
    if (cr0 == 0x95) {
      break;
    }
    HAL_Delay(10);
  } while (--retries > 0);

  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4);
  write_duty(0.0f); /* riscaldatore spento finche' non arriva un comando */
  mode = HEATER_OFF;
  last_command_tick = HAL_GetTick();
  last_pid_tick = last_command_tick;
}

static void enter_fault(void) {
  mode = HEATER_FAULT;
  write_duty(0.0f);
  pid_integral = 0.0f;
}

/* Safety check indipendente dal PID: fault MAX31856, T oltre cutoff, o
 * lettura stantia -> forza PWM a 0% e latcha fault. Stessa filosofia
 * della protezione INA301 R-01 di AntiSEL: sicurezza locale al dispositivo,
 * non dipendente dalla GUI/rete. */
static bool safety_check(void) {
  max31856_read_fault(&hmax);
  if (hmax.sr.val != 0U) {
    max31856_clear_fault_status(&hmax);
    enter_fault();
    return false;
  }
  if (!Heater_RtdFresh()) {
    enter_fault();
    return false;
  }
  if (latest_temp_c > cal.cutoff_c) {
    enter_fault();
    return false;
  }
  return true;
}

static void poll_rtd(void) {
  uint32_t now = HAL_GetTick();
  if (now - last_rtd_tick < 250U) {
    return;
  }
  last_rtd_tick = now;

  float temp_c = max31856_read_TC_temp(&hmax);
  latest_temp_c = temp_c;
  last_rtd_ok_tick = now;
  rtd_ever_ok = true;
}

static void run_pid(void) {
  uint32_t now = HAL_GetTick();
  float dt_s = (float)(now - last_pid_tick) / 1000.0f;
  if (dt_s < 0.05f) {
    return; /* aggiorna al massimo a ~20 Hz */
  }
  last_pid_tick = now;

  float error = setpoint_c - latest_temp_c;
  pid_integral += error * dt_s;

  /* Anti-windup: limita il contributo integrale da solo a +-50% di duty */
  float i_max = (cal.ki > 0.0f) ? (50.0f / cal.ki) : 0.0f;
  if (pid_integral > i_max) {
    pid_integral = i_max;
  } else if (pid_integral < -i_max) {
    pid_integral = -i_max;
  }

  /* Derivata sulla misura (non sull'errore): evita il "derivative kick"
   * quando il setpoint cambia di scatto. */
  float d_temp = (latest_temp_c - pid_prev_temp_c) / dt_s;
  pid_prev_temp_c = latest_temp_c;

  float out = (cal.kp * error) + (cal.ki * pid_integral) - (cal.kd * d_temp);
  write_duty(out);
}

static void run_autotune(void) {
  uint32_t now = HAL_GetTick();
  float dt_s = (float)(now - last_pid_tick) / 1000.0f;
  if (dt_s < 0.05f) {
    return;
  }
  last_pid_tick = now;

  if (latest_temp_c > autotune_peak_high) autotune_peak_high = latest_temp_c;
  if (latest_temp_c < autotune_peak_low) autotune_peak_low = latest_temp_c;

  switch (autotune_state) {
    case 0: /* Riscaldamento iniziale fino al setpoint */
      write_duty(100.0f);
      if (latest_temp_c >= setpoint_c) {
        autotune_state = 1;
        autotune_peak_high = latest_temp_c;
        autotune_peak_low = latest_temp_c;
        write_duty(0.0f);
      }
      break;
    case 1: /* Raffreddamento oltre il setpoint (inizio del primo ciclo utile) */
      write_duty(0.0f);
      if (latest_temp_c <= setpoint_c) {
        autotune_state = 2;
        autotune_t1 = now;
        write_duty(100.0f);
      }
      break;
    case 2: /* Riscaldamento, primo mezzo ciclo */
      write_duty(100.0f);
      if (latest_temp_c >= setpoint_c) {
        autotune_state = 3;
        autotune_t2 = now;
        write_duty(0.0f);
      }
      break;
    case 3: /* Raffreddamento, fine ciclo completo */
      write_duty(0.0f);
      if (latest_temp_c <= setpoint_c) {
        autotune_t3 = now;
        autotune_cycles++;
        if (autotune_cycles >= 3) {
          /* Calcolo Ziegler-Nichols */
          float a = (autotune_peak_high - autotune_peak_low) / 2.0f;
          if (a < 0.1f) a = 0.1f;
          float pu = (float)(autotune_t3 - autotune_t1) / 1000.0f;
          float ku = (4.0f * 50.0f) / (3.14159f * a);

          cal.kp = 0.6f * ku;
          cal.ki = 1.2f * ku / pu;
          cal.kd = 0.075f * ku * pu;

          /* Terminato, torna in OFF. I parametri restano in RAM */
          Heater_Off();
        } else {
          /* Prossimo ciclo */
          autotune_state = 2;
          autotune_t1 = autotune_t3;
          autotune_peak_high = latest_temp_c;
          autotune_peak_low = latest_temp_c;
          write_duty(100.0f);
        }
      }
      break;
  }
}

void Heater_Service(void) {
  poll_rtd();

  if (mode == HEATER_FAULT) {
    /* resta in fault finche' non arriva un ACK esplicito */
    return;
  }

  if (!safety_check()) {
    return;
  }

  if (mode == HEATER_AUTO) {
    if (HAL_GetTick() - last_command_tick > cal.watchdog_ms) {
      Heater_Off();
      return;
    }
    run_pid();
  } else if (mode == HEATER_AUTOTUNE) {
    if (HAL_GetTick() - last_command_tick > cal.watchdog_ms) {
      Heater_Off();
      return;
    }
    run_autotune();
  } else if (mode == HEATER_MANUAL) {
    write_duty(manual_duty_pct);
  } else {
    write_duty(0.0f);
  }
}

void Heater_SetSetpointC(float sp) {
  setpoint_c = sp;
  mode = HEATER_AUTO;
  pid_integral = 0.0f;
  last_command_tick = HAL_GetTick();
}

void Heater_StartAutotune(float sp) {
  setpoint_c = sp;
  mode = HEATER_AUTOTUNE;
  autotune_state = 0;
  autotune_peak_high = 0.0f;
  autotune_peak_low = 1000.0f;
  autotune_cycles = 0;
  last_command_tick = HAL_GetTick();
}

void Heater_SetManualDuty(float pct) {
  manual_duty_pct = pct;
  mode = HEATER_MANUAL;
  last_command_tick = HAL_GetTick();
}

void Heater_Off(void) {
  mode = HEATER_OFF;
  write_duty(0.0f);
  pid_integral = 0.0f;
  last_command_tick = HAL_GetTick();
}

void Heater_AckFault(void) {
  if (mode == HEATER_FAULT) {
    mode = HEATER_OFF;
  }
  last_command_tick = HAL_GetTick();
}

void Heater_KeepAlive(void) {
  last_command_tick = HAL_GetTick();
}

float Heater_LastTempC(void) { return latest_temp_c; }
float Heater_DutyPct(void) { return duty_pct; }
HeaterMode_t Heater_GetMode(void) { return mode; }

const char *Heater_ModeName(HeaterMode_t m) {
  switch (m) {
  case HEATER_OFF: return "OFF";
  case HEATER_MANUAL: return "MANUAL";
  case HEATER_AUTO: return "AUTO";
  case HEATER_AUTOTUNE: return "AUTOTUNE";
  case HEATER_FAULT: return "FAULT";
  default: return "?";
  }
}

bool Heater_RtdFresh(void) {
  return rtd_ever_ok && ((HAL_GetTick() - last_rtd_ok_tick) < cal.rtd_stale_ms);
}
