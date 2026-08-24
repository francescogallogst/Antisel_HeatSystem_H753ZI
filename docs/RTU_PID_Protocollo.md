# Protocollo RTU/PID Heat System — v1 (placeholder)

Documento di contratto tra **firmware** di `Antisel_HeatSystem` (server TCP)
e il pannello "PID CTRL + RTU" della dashboard Python
(`AntiSel_GUI/antisel_dashboard_eth.py`). Stesso stile di
[AntiSEL_Protocollo_Comandi.md](../../AntiSel_GUI/docs/AntiSEL_Protocollo_Comandi.md),
ma per il dispositivo Heat System, indipendente dalla scheda AntiSEL.

Vedi anche `AntiSEL/docs/Proposta_HeatSystem_RTU_PID.md` per il contesto
architetturale completo (§5.3 in particolare).

- **Trasporto:** TCP, `192.168.1.101:7756`
- **Encoding:** ASCII, un comando per riga, terminatore `\r\n`
- **Risposte:** una riga `OK ...` oppure `ERR <causa>`
- **Parsing GUI:** `_parse_kv()` estrae qualunque token `CHIAVE=VALORE`
  separato da spazi nella riga — l'ordine e il prefisso `OK`/testo
  circostante non contano, contano solo le coppie chiave=valore

Implementato in `Core/Src/rtu_protocol.c`.

---

## Comandi

| Comando | Argomento | Risposta OK | Note |
|---|---|---|---|
| `GET TEMP` | — | `OK TEMP=<°C, 1 decimale>` | `ERR RTD_STALE` se nessuna lettura RTD valida negli ultimi `rtd_stale_ms` (default 2000 ms, vedi `heater_ctrl.h`) |
| `GET PID` | — | `OK PID PWM=<%, 1 decimale> STATE=<OFF\|MANUAL\|AUTO\|FAULT>` | |
| `SET SETPOINT_C <v>` | setpoint [°C] | `OK SETPOINT_C=<v>` | Porta il controllore in modo **AUTO** e resetta il timestamp del watchdog di disconnessione |
| altro | — | `ERR UNKNOWN` | |

La GUI interroga `GET TEMP` e `GET PID` ogni `RTU_POLL_S` (1.0 s, definito
lato GUI) e invia `SET SETPOINT_C` quando l'utente preme "Imposta" nel
pannello RTU/PID.

## Stati (`STATE=`)

| Stato | Significato |
|---|---|
| `OFF` | Riscaldatore spento, nessun comando attivo |
| `MANUAL` | Duty impostato manualmente (non ancora raggiungibile dal protocollo attuale — riservato per estensioni future) |
| `AUTO` | PID attivo, insegue `SETPOINT_C` |
| `FAULT` | Safety check ha rilevato un problema (fault MAX31865, sovratemperatura, RTD stantia) — PWM forzato a 0%, resta in FAULT finché non arriva un `ACK FAULT` (comando non ancora esposto via rete, vedi §7 sotto) |

## Sicurezza locale (indipendente dal protocollo)

Implementata in `heater_ctrl.c`, non richiede comandi dalla GUI:

- **Safety check** ad ogni ciclo: se il MAX31865 segnala un fault, se la
  temperatura supera `cutoff_c` (default 90.0 °C, placeholder), o se la
  lettura RTD è stantia, il PWM viene forzato a 0% e lo stato passa a
  `FAULT`.
- **Watchdog di disconnessione**: se in modo `AUTO` non arriva nessun
  comando (`SET SETPOINT_C`) entro `watchdog_ms` (default 5000 ms,
  placeholder) il riscaldatore torna a `OFF` da solo, senza bisogno di un
  comando esplicito dalla GUI.

## Estensioni previste (non ancora implementate)

Come da proposta §5.3, il protocollo è pensato per essere esteso restando
compatibile col parsing esistente:

- `ACK FAULT` — uscita esplicita dallo stato FAULT (funzione
  `Heater_AckFault()` già presente nel driver, non ancora esposta via TCP)
- `SET MODE <OFF|MANUAL|AUTO>` — cambio modo esplicito
- `GET FAULT` — dettaglio bit di fault MAX31865

## Parametri ancora da tarare (§7 della proposta)

Questi valori sono **placeholder conservativi**, non tarati su hardware
reale — vedi i commenti in `heater_ctrl.c`:

| Parametro | Default attuale | Dove |
|---|---|---|
| Guadagni PID (Kp, Ki, Kd) | 5.0 / 0.05 / 0.0 | `heater_ctrl.c`, `cal` |
| Soglia di cutoff termico | 90.0 °C | `heater_ctrl.c`, `cal.cutoff_c` |
| Timeout watchdog disconnessione | 5000 ms | `heater_ctrl.c`, `cal.watchdog_ms` |
| Staleness RTD | 2000 ms | `heater_ctrl.c`, `cal.rtd_stale_ms` |
| Wiring RTD | 3 fili, PT100, R_REF=430 Ω | `max31865.c`, `cal` |
