/**
 ******************************************************************************
 * @file    heater_ctrl.c
 * @brief   Controllo riscaldatore: PID + PWM (TIM3_CH1) + sicurezza locale.
 ******************************************************************************
 */
#include "heater_ctrl.h"
#include "main.h"
#include "tim.h"
#include "max31865.h"

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
  uint32_t period = htim3.Init.Period; /* es. 65535 */
  uint32_t counts = (uint32_t)((pct / 100.0f) * (float)period);
  /* clamp esplicito: CCR e' un registro a 16 bit, un valore di 'period+1'
   * (100% "esatto") lo farebbe traboccare a 0, cioe' 0% invece di 100% */
  if (counts > period) {
    counts = period;
  }
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, counts);
}

void Heater_Init(void) {
  MAX31865_Init();
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
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

/* Safety check indipendente dal PID: fault MAX31865, T oltre cutoff, o
 * lettura RTD stantia -> forza PWM a 0% e latcha fault. Stessa filosofia
 * della protezione INA301 R-01 di AntiSEL: sicurezza locale al dispositivo,
 * non dipendente dalla GUI/rete. */
static bool safety_check(void) {
  MAX31865_Fault_t fault;
  if (MAX31865_ReadFaultStatus(&fault) && fault.raw != 0U) {
    MAX31865_ClearFault();
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

  uint16_t code;
  if (MAX31865_ReadRtd(&code)) {
    float ohms = MAX31865_CodeToOhms(code);
    latest_temp_c = MAX31865_OhmsToCelsius(ohms);
    last_rtd_ok_tick = now;
    rtd_ever_ok = true;
  }
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
      /* watchdog di disconnessione: nessun comando/keepalive dalla GUI
       * entro il timeout mentre si e' in AUTO -> spegnimento locale */
      Heater_Off();
      return;
    }
    run_pid();
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

float Heater_LastTempC(void) { return latest_temp_c; }
float Heater_DutyPct(void) { return duty_pct; }
HeaterMode_t Heater_GetMode(void) { return mode; }

const char *Heater_ModeName(HeaterMode_t m) {
  switch (m) {
  case HEATER_OFF: return "OFF";
  case HEATER_MANUAL: return "MANUAL";
  case HEATER_AUTO: return "AUTO";
  case HEATER_FAULT: return "FAULT";
  default: return "?";
  }
}

bool Heater_RtdFresh(void) {
  return rtd_ever_ok && ((HAL_GetTick() - last_rtd_ok_tick) < cal.rtd_stale_ms);
}
