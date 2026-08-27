/**
 ******************************************************************************
 * @file    rtu_protocol.c
 * @brief   Server TCP RTU/PID (porta 7756) — GET TEMP / GET PID /
 *          SET SETPOINT_C, compatibile col parsing _parse_kv() di
 *          antisel_dashboard_eth.py. Stesso pattern (LwIP raw TCP + dispatch
 *          a strncmp) di CM7/Src/antisel_protocol.c in AntiSEL.
 ******************************************************************************
 */
#include "rtu_protocol.h"
#include "heater_ctrl.h"
#include "max31856.h"
#include "main.h"
#include "lwip/tcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern max31856_t hmax;

static struct tcp_pcb *server_pcb;
static struct tcp_pcb *client_pcb;

static void send_str(struct tcp_pcb *tpcb, const char *s) {
  if (tcp_sndbuf(tpcb) >= strlen(s)) {
    tcp_write(tpcb, s, (u16_t)strlen(s), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
  }
}

/* ── comandi ────────────────────────────────────────────────────────────── */
static void handle_command(struct tcp_pcb *tpcb, char *line) {
  char buf[96];

  if (strncmp(line, "GET TEMP", 8) == 0) {
    Heater_KeepAlive();
    if (Heater_RtdFresh()) {
      float t = Heater_LastTempC();
      int t_i = (int)t;
      int t_f = (int)(((t - (float)t_i) * 10.0f));
      if (t_f < 0) {
        t_f = -t_f;
      }
      snprintf(buf, sizeof(buf), "OK TEMP=%d.%d\r\n", t_i, t_f);
    } else {
      snprintf(buf, sizeof(buf), "ERR RTD_STALE\r\n");
    }
    send_str(tpcb, buf);
    return;
  }

  if (strncmp(line, "GET PID", 7) == 0) {
    Heater_KeepAlive();
    float pwm = Heater_DutyPct();
    int p_i = (int)pwm;
    int p_f = (int)(((pwm - (float)p_i) * 10.0f));
    snprintf(buf, sizeof(buf), "OK PID PWM=%d.%d STATE=%s\r\n", p_i, p_f,
             Heater_ModeName(Heater_GetMode()));
    send_str(tpcb, buf);
    return;
  }

  if (strncmp(line, "SET SETPOINT_C", 14) == 0) {
    float sp = (float)atof(line + 14);
    Heater_SetSetpointC(sp);
    int s_i = (int)sp;
    int s_f = (int)(((sp - (float)s_i) * 10.0f));
    snprintf(buf, sizeof(buf), "OK SETPOINT_C=%d.%d\r\n", s_i, s_f);
    send_str(tpcb, buf);
    return;
  }

  if (strncmp(line, "SET AUTOTUNE_C", 14) == 0) {
    float sp = (float)atof(line + 14);
    Heater_StartAutotune(sp);
    int s_i = (int)sp;
    int s_f = (int)(((sp - (float)s_i) * 10.0f));
    snprintf(buf, sizeof(buf), "OK AUTOTUNE_C=%d.%d\r\n", s_i, s_f);
    send_str(tpcb, buf);
    return;
  }

  if (strncmp(line, "GET RAW", 7) == 0) {
    uint8_t cr0  = max31856_read_register(&hmax, MAX31856_CR0);
    uint8_t cr1  = max31856_read_register(&hmax, MAX31856_CR1);
    uint8_t mask = max31856_read_register(&hmax, MAX31856_MASK);
    uint8_t sr   = max31856_read_register(&hmax, MAX31856_SR);
    uint8_t ltcb[3];
    max31856_read_nregisters(&hmax, MAX31856_LTCBH, ltcb, 3);
    
    snprintf(buf, sizeof(buf),
             "OK RAW CR0=%02X CR1=%02X MASK=%02X SR=%02X LTCB=%02X%02X%02X\r\n",
             cr0, cr1, mask, sr, ltcb[0], ltcb[1], ltcb[2]);
    send_str(tpcb, buf);
    return;
  }

  if (strncmp(line, "GET GPIO", 8) == 0) {
    send_str(tpcb, "ERR UNSUPPORTED_BY_NEW_DRIVER\r\n");
    return;
  }

  if (strncmp(line, "TEST SCK", 8) == 0) {
    send_str(tpcb, "ERR UNSUPPORTED_BY_NEW_DRIVER\r\n");
    return;
  }

  if (strncmp(line, "TEST MOSI", 9) == 0) {
    send_str(tpcb, "ERR UNSUPPORTED_BY_NEW_DRIVER\r\n");
    return;
  }

  if (strncmp(line, "TEST LOOP", 9) == 0) {
    send_str(tpcb, "ERR UNSUPPORTED_BY_NEW_DRIVER\r\n");
    return;
  }

  if (strncmp(line, "ACK FAULT", 9) == 0) {
    Heater_AckFault();
    snprintf(buf, sizeof(buf), "OK ACK STATE=%s\r\n", Heater_ModeName(Heater_GetMode()));
    send_str(tpcb, buf);
    return;
  }

  send_str(tpcb, "ERR UNKNOWN\r\n");
}

/* ── TCP server ─────────────────────────────────────────────────────────── */
static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
                      err_t err) {
  static char line_buf[64];
  static uint8_t line_len;
  (void)arg;
  (void)err;

  if (p == NULL) {
    client_pcb = NULL;
    line_len = 0U;
    tcp_close(tpcb);
    return ERR_OK;
  }
  for (struct pbuf *q = p; q != NULL; q = q->next) {
    const char *d = (const char *)q->payload;
    for (u16_t i = 0; i < q->len; i++) {
      char c = d[i];
      if (c == '\n' || c == '\r') {
        if (line_len > 0U) {
          line_buf[line_len] = '\0';
          handle_command(tpcb, line_buf);
          line_len = 0U;
        }
      } else if (line_len < sizeof(line_buf) - 1U) {
        line_buf[line_len++] = c;
      } else {
        line_len = 0U; /* riga troppo lunga: scarta */
      }
    }
  }
  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

static void on_err(void *arg, err_t err) {
  (void)arg;
  (void)err;
  client_pcb = NULL;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
  (void)arg;
  if (err != ERR_OK || newpcb == NULL) {
    return ERR_VAL;
  }
  /* un solo client alla volta (line_buf di on_recv e' condiviso, non
   * per-connessione) — stesso vincolo di antisel_protocol.c */
  if (client_pcb != NULL) {
    tcp_abort(client_pcb);
  }
  client_pcb = newpcb;
  tcp_setprio(newpcb, TCP_PRIO_MIN);
  tcp_recv(newpcb, on_recv);
  tcp_err(newpcb, on_err);
  return ERR_OK;
}

void RTU_Init(void) {
  server_pcb = tcp_new();
  if (server_pcb == NULL) {
    return;
  }
  tcp_bind(server_pcb, IP_ADDR_ANY, RTU_PROTOCOL_PORT);
  server_pcb = tcp_listen_with_backlog(server_pcb, 1);
  if (server_pcb == NULL) {
    return;
  }
  tcp_accept(server_pcb, on_accept);
}
